/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/http_transport.h"

#include <QtCore/QPointer>
#include <QtCore/QThread>

namespace MTP {
namespace {

class BlockedReply final : public QNetworkReply {
public:
	BlockedReply(QNetworkAccessManager *parent, const QNetworkRequest &request);
	void abort() override;

protected:
	qint64 readData(char *data, qint64 maxSize) override;

};

BlockedReply::BlockedReply(
		QNetworkAccessManager *parent,
		const QNetworkRequest &request)
: QNetworkReply(parent) {
	setRequest(request);
	setUrl(request.url());
	open(QIODevice::ReadOnly);
	setError(QNetworkReply::ProxyNotFoundError, u"Account HTTP route unavailable"_q);
	setFinished(true);
	QMetaObject::invokeMethod(this, [=] {
		Q_EMIT errorOccurred(error());
		Q_EMIT finished();
	}, Qt::QueuedConnection);
}

void BlockedReply::abort() {
}

qint64 BlockedReply::readData(char *data, qint64 maxSize) {
	return -1;
}

} // namespace

HttpTransport::HttpTransport(NetworkSnapshot route)
: _route(std::move(route)) {
	if (const auto proxy = _route.httpProxy()) {
		setProxy(*proxy);
	}
	setRedirectPolicy(QNetworkRequest::UserVerifiedRedirectPolicy);
}

HttpTransport::HttpTransport(NetworkContext &context)
: HttpTransport(context.snapshot()) {
	context.changes() | rpl::on_next([=] { revoke(); }, _lifetime);
}

HttpTransport::~HttpTransport() {
	_lifetime.destroy();
	for (const auto reply : findChildren<QNetworkReply*>()) {
		reply->disconnect();
	}
	revoke();
}

QNetworkReply *HttpTransport::createRequest(
		Operation operation,
		const QNetworkRequest &request,
		QIODevice *outgoingData) {
	const auto scheme = request.url().scheme();
	if (_revoked || !_route.httpProxy()
		|| (scheme != u"http"_q && scheme != u"https"_q)) {
		return new BlockedReply(this, request);
	}
	const auto reply = QNetworkAccessManager::createRequest(
		operation,
		request,
		outgoingData);
	connect(reply, &QNetworkReply::redirected, reply, [=](const QUrl &target) {
		if (!_revoked && _route.httpProxy()
			&& (target.scheme() == u"http"_q || target.scheme() == u"https"_q)
			&& (reply->url().scheme() != u"https"_q
				|| target.scheme() == u"https"_q)) {
			reply->redirectAllowed();
		} else {
			reply->abort();
		}
	});
	return reply;
}

void HttpTransport::revoke() {
	Expects(thread() == QThread::currentThread());
	_revoked = true;
	auto replies = std::vector<QPointer<QNetworkReply>>();
	for (const auto reply : findChildren<QNetworkReply*>()) {
		replies.emplace_back(reply);
	}
	const auto guard = QPointer<HttpTransport>(this);
	for (const auto &reply : replies) {
		if (reply && !reply->isFinished()) {
			reply->abort();
			if (!guard) {
				return;
			}
		}
	}
	clearConnectionCache();
}

} // namespace MTP
