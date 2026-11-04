/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/proxy_check.h"

#include "mtproto/facade.h"
#include "mtproto/mtproto_dc_options.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QThread>
#include <atomic>

namespace MTP {
namespace {

using Connection = details::AbstractConnection;

class CheckExecutor final {
public:
	CheckExecutor();
	~CheckExecutor();

	[[nodiscard]] static std::shared_ptr<CheckExecutor> Acquire();
	void invoke(Fn<void()> callback);

private:
	QThread _thread;
	QObject _context;

};

struct CheckRequest {
	DcOptions::Variants::Protocol protocol = DcOptions::Variants::Tcp;
	QString address;
	int port = 0;
	bytes::vector secret;
	int16 dcId = 0;
};

CheckExecutor::CheckExecutor() {
	_thread.setObjectName(u"MTP Proxy Checks"_q);
	_context.moveToThread(&_thread);
	_thread.start();
}

CheckExecutor::~CheckExecutor() {
	invoke([&] {
		_context.moveToThread(QCoreApplication::instance()->thread());
	});
	_thread.quit();
	_thread.wait();
}

std::shared_ptr<CheckExecutor> CheckExecutor::Acquire() {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());

	static auto weak = std::weak_ptr<CheckExecutor>();
	auto result = weak.lock();
	if (!result) {
		result = std::make_shared<CheckExecutor>();
		weak = result;
	}
	return result;
}

void CheckExecutor::invoke(Fn<void()> callback) {
	Expects(QThread::currentThread() != &_thread);

	QMetaObject::invokeMethod(
		&_context,
		std::move(callback),
		Qt::BlockingQueuedConnection);
}

} // namespace

class ProxyCheckConnection::Private final {
public:
	Private(
		std::shared_ptr<CheckExecutor> executor,
		not_null<Instance*> instance,
		const ProxyData &proxy,
		const CheckRequest &request,
		Fn<void(Connection*, int)> done,
		Fn<void(Connection*)> fail);
	~Private();

	[[nodiscard]] Connection *get() const;

private:
	void start(
		not_null<Instance*> instance,
		const ProxyData &proxy,
		const CheckRequest &request,
		Fn<void(Connection*, int)> done,
		Fn<void(Connection*)> fail);

	const std::shared_ptr<CheckExecutor> _executor;
	const std::shared_ptr<std::atomic<bool>> _alive;
	details::ConnectionPointer _connection;

};

ProxyCheckConnection::Private::Private(
	std::shared_ptr<CheckExecutor> executor,
	not_null<Instance*> instance,
	const ProxyData &proxy,
	const CheckRequest &request,
	Fn<void(Connection*, int)> done,
	Fn<void(Connection*)> fail)
: _executor(std::move(executor))
, _alive(std::make_shared<std::atomic<bool>>(true)) {
	_executor->invoke([&] {
		start(instance, proxy, request, std::move(done), std::move(fail));
	});
}

void ProxyCheckConnection::Private::start(
		not_null<Instance*> instance,
		const ProxyData &proxy,
		const CheckRequest &request,
		Fn<void(Connection*, int)> done,
		Fn<void(Connection*)> fail) {
	const auto alive = _alive;
	_connection = Connection::Create(
		instance,
		request.protocol,
		QThread::currentThread(),
		request.secret,
		proxy);
	const auto raw = _connection.get();
	QObject::connect(raw, &Connection::connected, raw, [=] {
		const auto ping = int(raw->pingTime());
		crl::on_main([=] {
			if (alive->load() && done) {
				done(raw, ping);
			}
		});
	});
	const auto failed = [=] {
		crl::on_main([=] {
			if (alive->load() && fail) {
				fail(raw);
			}
		});
	};
	QObject::connect(raw, &Connection::disconnected, raw, failed);
	QObject::connect(raw, &Connection::error, raw, failed);
	raw->connectToServer(
		request.address,
		request.port,
		request.secret,
		request.dcId,
		false);
}

ProxyCheckConnection::Private::~Private() {
	_alive->store(false);
	_executor->invoke([&] {
		const auto raw = _connection.get();
		_connection = nullptr;
		QCoreApplication::sendPostedEvents(raw, QEvent::DeferredDelete);
	});
}

Connection *ProxyCheckConnection::Private::get() const {
	return _connection.get();
}

ProxyCheckConnection::ProxyCheckConnection() = default;

ProxyCheckConnection::ProxyCheckConnection(ProxyCheckConnection &&other)
	= default;

ProxyCheckConnection &ProxyCheckConnection::operator=(
	ProxyCheckConnection &&other) = default;

ProxyCheckConnection &ProxyCheckConnection::operator=(std::nullptr_t) {
	_private = nullptr;
	return *this;
}

ProxyCheckConnection::~ProxyCheckConnection() = default;

Connection *ProxyCheckConnection::get() const {
	return _private ? _private->get() : nullptr;
}

ProxyCheckConnection::operator bool() const {
	return get() != nullptr;
}

void ResetProxyCheckers(
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6) {
	v4 = nullptr;
	v6 = nullptr;
}

void DropProxyChecker(
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6,
		not_null<Connection*> raw) {
	if (v4.get() == raw) {
		v4 = nullptr;
	} else if (v6.get() == raw) {
		v6 = nullptr;
	}
}

bool HasProxyCheckers(
		const ProxyCheckConnection &v4,
		const ProxyCheckConnection &v6) {
	return v4 || v6;
}

void StartProxyCheck(
		not_null<Instance*> mtproto,
		const ProxyData &proxy,
		bool tryIPv6,
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6,
		Fn<void(Connection *raw, int ping)> done,
		Fn<void(Connection *raw)> fail) {
	using Variants = DcOptions::Variants;

	if (proxy.type == ProxyData::Type::Web) {
		ResetProxyCheckers(v4, v6);
		return;
	}
	const auto executor = CheckExecutor::Acquire();
	ResetProxyCheckers(v4, v6);
	const auto connType = (proxy.type == ProxyData::Type::Http)
		? Variants::Http
		: Variants::Tcp;
	const auto dcId = mtproto->mainDcId();
	const auto setup = [&](ProxyCheckConnection &checker, CheckRequest request) {
		checker._private = std::make_unique<ProxyCheckConnection::Private>(
			executor,
			mtproto,
			proxy,
			request,
			done,
			fail);
	};
	if (proxy.type == ProxyData::Type::Mtproto) {
		setup(v4, {
			.protocol = connType,
			.address = proxy.host,
			.port = int(proxy.port),
			.secret = proxy.secretFromMtprotoPassword(),
			.dcId = int16(dcId),
		});
		return;
	}
	const auto options = mtproto->dcOptions().lookup(
		dcId,
		DcType::Regular,
		true);
	const auto tryConnect = [&](ProxyCheckConnection &checker, Variants::Address address) {
		const auto &list = options.data[address][connType];
		if (list.empty() || ((address == Variants::IPv6) && !tryIPv6)) {
			return;
		}
		const auto &endpoint = list.front();
		setup(checker, {
			.protocol = connType,
			.address = QString::fromStdString(endpoint.ip),
			.port = endpoint.port,
			.secret = endpoint.secret,
			.dcId = int16(dcId),
		});
	};
	tryConnect(v4, Variants::IPv4);
	tryConnect(v6, Variants::IPv6);
}

} // namespace MTP
