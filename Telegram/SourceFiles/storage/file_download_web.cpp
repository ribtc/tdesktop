/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_download_web.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/http_transport.h"
#include "storage/cache/storage_cache_types.h"

#include <QtNetwork/QAuthenticator>

namespace {

constexpr auto kMaxWebFileQueries = 8;
constexpr auto kMaxHttpRedirects = 5;
constexpr auto kResetDownloadPrioritiesTimeout = crl::time(200);
constexpr auto kMaxWebFile = 4000 * int64(1024 * 1024);

enum class ProcessResult {
	Error,
	Progress,
	Finished,
};

enum class Error {
};

struct Progress {
	qint64 ready = 0;
	qint64 total = 0;
	QByteArray streamed;
};

using Update = std::variant<Progress, QByteArray, Error>;

struct UpdateForLoader {
	not_null<webFileLoader*> loader;
	Update data;
};

// Only http(s) requests are ever performed by this loader. Otherwise a
// web content URL (or a redirect target chosen by some server) could be
// a file: URL, which on Windows opens UNC paths like \\host\share\file
// and discloses the user's NTLM credentials to that host.
[[nodiscard]] bool IsSupportedWebUrl(const QUrl &url) {
	const auto scheme = url.scheme();
	return url.isValid()
		&& (scheme == QLatin1String("http")
			|| scheme == QLatin1String("https"));
}

} // namespace

class WebLoadManager final
: public base::has_weak_ptr
, public std::enable_shared_from_this<WebLoadManager> {
public:
	explicit WebLoadManager(MTP::NetworkContext &context);
	~WebLoadManager();

	void enqueue(not_null<webFileLoader*> loader);
	void remove(not_null<webFileLoader*> loader);

	[[nodiscard]] rpl::producer<Update> updates(
		not_null<webFileLoader*> loader) const;

private:
	struct Enqueued {
		int id = 0;
		QString url;
		bool stream = false;
	};
	struct Sent {
		QString url;
		not_null<QNetworkReply*> reply;
		bool stream = false;
		QByteArray data;
		int64 ready = 0;
		int64 total = 0;
		int redirectsLeft = kMaxHttpRedirects;
	};

	// Constructor.
	void handleNetworkErrors();

	// Worker thread.
	void enqueue(int id, const QString &url, bool stream);
	void remove(int id);
	void resetGeneration();
	void checkSendNext();
	void send(const Enqueued &entry);
	[[nodiscard]] QNetworkReply *send(int id, const QString &url);
	[[nodiscard]] Sent *findSent(int id, not_null<QNetworkReply*> reply);
	void removeSent(int id);
	void progress(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total);
	void failed(
		int id,
		not_null<QNetworkReply*> reply,
		QNetworkReply::NetworkError error);
	void redirect(int id, not_null<QNetworkReply*> reply);
	void notify(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total);
	void failed(int id, not_null<QNetworkReply*> reply);
	void finished(int id, not_null<QNetworkReply*> reply);
	void deleteDeferred(not_null<QNetworkReply*> reply);
	void queueProgressUpdate(
		int id,
		int64 ready,
		int64 total,
		QByteArray streamed);
	void queueFailedUpdate(int id);
	void queueFinishedUpdate(int id, const QByteArray &data);
	void clear();
	void revoke();

	// Main thread.
	void sendUpdate(int id, Update &&data);

	const MTP::NetworkSnapshot _route;
	rpl::lifetime _routeLifetime;
	QThread _thread;
	std::unique_ptr<MTP::HttpTransport> _network;
	base::Timer _resetGenerationTimer;

	// Main thread.
	rpl::event_stream<UpdateForLoader> _updates;
	int _autoincrement = 0;
	base::flat_map<not_null<webFileLoader*>, int> _ids;

	// Worker thread.
	std::deque<Enqueued> _queue;
	std::deque<Enqueued> _previousGeneration;
	base::flat_map<int, Sent> _sent;
	std::vector<QPointer<QNetworkReply>> _repliesBeingDeleted;

};

namespace {

struct AccountLoadManager {
	MTP::NetworkContext *context = nullptr;
	uint64 generation = 0;
	std::weak_ptr<WebLoadManager> manager;
};

std::vector<AccountLoadManager> AccountLoadManagers;

[[nodiscard]] std::shared_ptr<WebLoadManager> GetManager(
		MTP::NetworkContext &context) {
	std::erase_if(AccountLoadManagers, [](const AccountLoadManager &entry) {
		return entry.manager.expired();
	});
	const auto route = context.snapshot();
	if (!route.httpProxy()) {
		return nullptr;
	}
	for (const auto &entry : AccountLoadManagers) {
		if (entry.context == &context && entry.generation == route.generation) {
			return entry.manager.lock();
		}
	}
	auto result = std::make_shared<WebLoadManager>(context);
	AccountLoadManagers.push_back({ &context, route.generation, result });
	return result;
}

} // namespace

WebLoadManager::WebLoadManager(MTP::NetworkContext &context)
: _route(context.snapshot())
, _network(std::make_unique<MTP::HttpTransport>(_route))
, _resetGenerationTimer(&_thread, [=] { resetGeneration(); }) {
	_network->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
	handleNetworkErrors();

	_network->moveToThread(&_thread);
	QObject::connect(&_thread, &QThread::finished, [=] {
		clear();
		_network = nullptr;
	});
	_thread.start();
	context.changes() | rpl::on_next([=] { revoke(); }, _routeLifetime);
}

void WebLoadManager::revoke() {
	const auto guard = shared_from_this();
	QMetaObject::invokeMethod(_network.get(), [=] {
		_resetGenerationTimer.cancel();
		_queue.clear();
		_previousGeneration.clear();
		clear();
		_network->revoke();
	}, Qt::BlockingQueuedConnection);
	const auto ids = _ids;
	for (const auto &[loader, id] : ids) {
		sendUpdate(id, Error{});
	}
}

void WebLoadManager::handleNetworkErrors() {
	const auto fail = [=](QNetworkReply *reply) {
		for (const auto &[id, sent] : _sent) {
			if (sent.reply == reply) {
				failed(id, reply);
				return;
			}
		}
	};
	QObject::connect(
		_network.get(),
		&QNetworkAccessManager::authenticationRequired,
		fail);
	QObject::connect(
		_network.get(),
		&QNetworkAccessManager::sslErrors,
		fail);
}

WebLoadManager::~WebLoadManager() {
	_routeLifetime.destroy();
	_thread.quit();
	_thread.wait();
}

[[nodiscard]] rpl::producer<Update> WebLoadManager::updates(
		not_null<webFileLoader*> loader) const {
	return _updates.events(
	) | rpl::filter([=](const UpdateForLoader &update) {
		return (update.loader == loader);
	}) | rpl::map([=](UpdateForLoader &&update) {
		return std::move(update.data);
	});
}

void WebLoadManager::enqueue(not_null<webFileLoader*> loader) {
	const auto id = [&] {
		const auto i = _ids.find(loader);
		return (i != end(_ids))
			? i->second
			: _ids.emplace(loader, ++_autoincrement).first->second;
	}();
	const auto url = loader->url();
	const auto stream = loader->streamLoading();
	InvokeQueued(_network.get(), [=] {
		enqueue(id, url, stream);
	});
}

void WebLoadManager::remove(not_null<webFileLoader*> loader) {
	const auto i = _ids.find(loader);
	if (i == end(_ids)) {
		return;
	}
	const auto id = i->second;
	_ids.erase(i);
	InvokeQueued(_network.get(), [=] {
		remove(id);
	});
}

void WebLoadManager::enqueue(int id, const QString &url, bool stream) {
	if (!_route.allowed()) {
		queueFailedUpdate(id);
		return;
	}
	const auto i = ranges::find(_queue, id, &Enqueued::id);
	if (i != end(_queue)) {
		return;
	}
	if (!IsSupportedWebUrl(QUrl(url))) {
		LOG(("Network Error: "
			"Bad url requested in WebLoadManager::enqueue(): '%1'"
			).arg(url));
		queueFailedUpdate(id);
		return;
	}
	_previousGeneration.erase(
		ranges::remove(_previousGeneration, id, &Enqueued::id),
		end(_previousGeneration));
	_queue.push_back(Enqueued{ id, url, stream });
	if (!_resetGenerationTimer.isActive()) {
		_resetGenerationTimer.callOnce(kResetDownloadPrioritiesTimeout);
	}
	checkSendNext();
}

void WebLoadManager::remove(int id) {
	_queue.erase(ranges::remove(_queue, id, &Enqueued::id), end(_queue));
	_previousGeneration.erase(
		ranges::remove(_previousGeneration, id, &Enqueued::id),
		end(_previousGeneration));
	removeSent(id);
}

void WebLoadManager::resetGeneration() {
	if (!_previousGeneration.empty()) {
		std::copy(
			begin(_previousGeneration),
			end(_previousGeneration),
			std::back_inserter(_queue));
		_previousGeneration.clear();
	}
	std::swap(_queue, _previousGeneration);
}

void WebLoadManager::checkSendNext() {
	if (!_route.allowed()
		|| _sent.size() >= kMaxWebFileQueries
		|| (_queue.empty() && _previousGeneration.empty())) {
		return;
	}
	const auto entry = _queue.empty()
		? _previousGeneration.front()
		: _queue.front();
	(_queue.empty() ? _previousGeneration : _queue).pop_front();
	send(entry);
}

void WebLoadManager::send(const Enqueued &entry) {
	const auto id = entry.id;
	const auto url = entry.url;
	if (const auto reply = send(id, url)) {
		_sent.emplace(id, Sent{ url, reply, entry.stream });
	} else {
		queueFailedUpdate(id);
	}
}

void WebLoadManager::removeSent(int id) {
	if (const auto i = _sent.find(id); i != end(_sent)) {
		deleteDeferred(i->second.reply);
		_sent.erase(i);
		checkSendNext();
	}
}

QNetworkReply *WebLoadManager::send(int id, const QString &url) {
	if (!_route.allowed()) {
		return nullptr;
	}
	const auto result = _network->get(QNetworkRequest(url));
	const auto handleProgress = [=](qint64 ready, qint64 total) {
		progress(id, result, ready, total);
	};
	const auto handleError = [=](QNetworkReply::NetworkError error) {
		failed(id, result, error);
	};
	QObject::connect(
		result,
		&QNetworkReply::downloadProgress,
		handleProgress);
	QObject::connect(result, &QNetworkReply::errorOccurred, handleError);
	return result;
}

WebLoadManager::Sent *WebLoadManager::findSent(
		int id,
		not_null<QNetworkReply*> reply) {
	const auto i = _sent.find(id);
	return (i != end(_sent) && i->second.reply == reply)
		? &i->second
		: nullptr;
}

void WebLoadManager::progress(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total) {
	if (total <= 0) {
		const auto originalContentLength = reply->attribute(
			QNetworkRequest::OriginalContentLengthAttribute);
		if (originalContentLength.isValid()) {
			total = originalContentLength.toLongLong();
		}
	}
	const auto statusCode = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute);
	const auto status = statusCode.isValid() ? statusCode.toInt() : 200;
	if (status == 301 || status == 302 || status == 303
		|| status == 307 || status == 308) {
		redirect(id, reply);
	} else if (status != 200 && status != 206 && status != 416) {
		LOG(("Network Error: "
			"Bad HTTP status received in WebLoadManager::onProgress() %1"
			).arg(status));
		failed(id, reply);
	} else {
		notify(id, reply, ready, std::max(ready, total));
	}
}

void WebLoadManager::redirect(int id, not_null<QNetworkReply*> reply) {
	// The cooked LocationHeader value is empty for relative targets,
	// because Qt parses it into a QUrl only when it carries a scheme.
	const auto raw = reply->rawHeader("Location");
	auto url = QUrl(QString::fromUtf8(raw));
	if (!url.isValid()) {
		url = QUrl(QString::fromLatin1(raw));
	}

	if (const auto sent = findSent(id, reply)) {
		const auto next = reply->url().resolved(url);
		if (raw.isEmpty() || !IsSupportedWebUrl(next)
			|| (reply->url().scheme() == u"https"_q
				&& next.scheme() != u"https"_q)) {
			LOG(("Network Error: "
				"Bad redirect target in WebLoadManager::redirect() "
				"for web file loader: %1").arg(QString::fromUtf8(raw)));
			failed(id, reply);
			return;
		} else if (!sent->redirectsLeft--) {
			LOG(("Network Error: "
				"Too many HTTP redirects in onFinished() "
				"for web file loader: %1").arg(next.toString()));
			failed(id, reply);
			return;
		}
		const auto target = next.toString();
		deleteDeferred(reply);
		sent->url = target;
		if (const auto nextReply = send(id, target)) {
			sent->reply = nextReply;
		} else {
			failed(id, reply);
		}
	}
}

void WebLoadManager::notify(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total) {
	if (const auto sent = findSent(id, reply)) {
		sent->ready = ready;
		sent->total = std::max(total, int64(0));
		if (total <= 0) {
			LOG(("Network Error: "
				"Bad size received for HTTP download progress "
				"in WebLoadManager::onProgress(): %1 / %2 (bytes %3)"
				).arg(ready
				).arg(total
				).arg(sent->data.size()));
			failed(id, reply);
			return;
		}
		auto bytes = reply->readAll();
		if (sent->stream) {
			if (total > kMaxWebFile) {
				LOG(("Network Error: "
					"Bad size received for HTTP download progress "
					"in WebLoadManager::onProgress(): %1 / %2"
					).arg(ready
					).arg(total));
				failed(id, reply);
			} else {
				queueProgressUpdate(
					id,
					sent->ready,
					sent->total,
					std::move(bytes));
				if (ready >= total) {
					finished(id, reply);
				}
			}
		} else {
			sent->data.append(std::move(bytes));
			if (total > Storage::kMaxFileInMemory
				|| sent->data.size() > Storage::kMaxFileInMemory) {
				LOG(("Network Error: "
					"Bad size received for HTTP download progress "
					"in WebLoadManager::onProgress(): %1 / %2 (bytes %3)"
					).arg(ready
					).arg(total
					).arg(sent->data.size()));
				failed(id, reply);
			} else if (ready >= total) {
				finished(id, reply);
			} else {
				queueProgressUpdate(id, sent->ready, sent->total, {});
			}
		}
	}
}

void WebLoadManager::failed(
		int id,
		not_null<QNetworkReply*> reply,
		QNetworkReply::NetworkError error) {
	if (const auto sent = findSent(id, reply)) {
		LOG(("Network Error: "
			"Failed to request '%1', error %2 (%3)"
			).arg(sent->url
			).arg(int(error)
			).arg(reply->errorString()));
		failed(id, reply);
	}
}

void WebLoadManager::failed(int id, not_null<QNetworkReply*> reply) {
	if ([[maybe_unused]] const auto sent = findSent(id, reply)) {
		removeSent(id);
		queueFailedUpdate(id);
	}
}

void WebLoadManager::deleteDeferred(not_null<QNetworkReply*> reply) {
	reply->disconnect();
	reply->abort();
	reply->deleteLater();
	_repliesBeingDeleted.erase(
		ranges::remove(_repliesBeingDeleted, nullptr),
		end(_repliesBeingDeleted));
	_repliesBeingDeleted.emplace_back(reply.get());
}

void WebLoadManager::finished(int id, not_null<QNetworkReply*> reply) {
	if (const auto sent = findSent(id, reply)) {
		const auto data = base::take(sent->data);
		removeSent(id);
		queueFinishedUpdate(id, data);
	}
}

void WebLoadManager::clear() {
	for (const auto &[id, sent] : base::take(_sent)) {
		sent.reply->disconnect();
		sent.reply->abort();
		delete sent.reply;
	}
	for (const auto &reply : base::take(_repliesBeingDeleted)) {
		if (reply) {
			delete reply;
		}
	}
}

void WebLoadManager::queueProgressUpdate(
		int id,
		int64 ready,
		int64 total,
		QByteArray streamed) {
	crl::on_main(this, [=, bytes = std::move(streamed)]() mutable {
		sendUpdate(id, Progress{ ready, total, std::move(bytes) });
	});
}

void WebLoadManager::queueFailedUpdate(int id) {
	crl::on_main(this, [=] {
		sendUpdate(id, Error{});
	});
}

void WebLoadManager::queueFinishedUpdate(int id, const QByteArray &data) {
	crl::on_main(this, [=] {
		for (const auto &[loader, loaderId] : _ids) {
			if (loaderId == id) {
				break;
			}
		}
		sendUpdate(id, QByteArray(data));
	});
}

void WebLoadManager::sendUpdate(int id, Update &&data) {
	const auto guard = shared_from_this();
	if (!_route.allowed()) {
		data = Error{};
	}
	for (const auto &[loader, loaderId] : _ids) {
		if (loaderId == id) {
			_updates.fire(UpdateForLoader{ loader, std::move(data) });
			return;
		}
	}
}

webFileLoader::webFileLoader(
	not_null<Main::Session*> session,
	const QString &url,
	const QString &to,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	0,
	0,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, _url(url) {
}

webFileLoader::webFileLoader(
	not_null<Main::Session*> session,
	const QString &url,
	const QString &path,
	WebRequestType type)
: FileLoader(
	session,
	path,
	0,
	0,
	UnknownFileLocation,
	LoadToFileOnly,
	LoadFromCloudOrLocal,
	false,
	0)
, _url(url)
, _requestType(type) {
}

webFileLoader::~webFileLoader() {
	if (!_finished) {
		cancel();
	}
}

QString webFileLoader::url() const {
	return _url;
}

WebRequestType webFileLoader::requestType() const {
	return _requestType;
}

bool webFileLoader::streamLoading() const {
	return (_toCache == LoadToFileOnly);
}

void webFileLoader::startLoading() {
	if (_finished) {
		return;
	} else if (!_manager) {
		_manager = GetManager(session().account().network());
		if (!_manager) {
			loadFailed();
			return;
		}
		_manager->updates(
			this
		) | rpl::on_next([=](const Update &data) {
			if (const auto progress = std::get_if<Progress>(&data)) {
				loadProgress(
					progress->ready,
					progress->total,
					progress->streamed);
			} else if (const auto bytes = std::get_if<QByteArray>(&data)) {
				loadFinished(*bytes);
			} else {
				loadFailed();
			}
		}, _managerLifetime);
	}
	_manager->enqueue(this);
}

int64 webFileLoader::currentOffset() const {
	return _ready;
}

void webFileLoader::loadProgress(
		qint64 ready,
		qint64 total,
		const QByteArray &streamed) {
	_fullSize = _loadSize = total;
	_ready = ready;
	if (!streamed.isEmpty()
		&& !writeResultPart(_streamedOffset, bytes::make_span(streamed))) {
		loadFailed();
	} else {
		_streamedOffset += streamed.size();
		notifyAboutProgress();
	}
}

void webFileLoader::loadFinished(const QByteArray &data) {
	cancelRequest();
	if (writeResultPart(0, bytes::make_span(data))) {
		finalizeResult();
	}
}

void webFileLoader::loadFailed() {
	const auto route = session().account().network().snapshot();
	cancel(!route.allowed()
		? FailureReason::NetworkBlocked
		: !route.httpProxy()
		? FailureReason::NetworkUnsupported
		: FailureReason::OtherFailure);
}

Storage::Cache::Key webFileLoader::cacheKey() const {
	return Data::UrlCacheKey(_url);
}

std::optional<MediaKey> webFileLoader::fileLocationKey() const {
	return std::nullopt;
}

void webFileLoader::cancelHook() {
	cancelRequest();
}

void webFileLoader::cancelRequest() {
	if (!_manager) {
		return;
	}
	_managerLifetime.destroy();
	_manager->remove(this);
	_manager = nullptr;
}
