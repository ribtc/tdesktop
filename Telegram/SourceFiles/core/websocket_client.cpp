/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/websocket_client.h"

#include "base/random.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QThread>
#include <QtNetwork/QSslSocket>

namespace Core {
namespace {

constexpr auto kOpcodeContinuation = quint8(0x0);
constexpr auto kOpcodeText = quint8(0x1);
constexpr auto kOpcodeBinary = quint8(0x2);
constexpr auto kOpcodeClose = quint8(0x8);
constexpr auto kOpcodePing = quint8(0x9);
constexpr auto kOpcodePong = quint8(0xA);

constexpr auto kMaxFramePayload = quint64(4 * 1024 * 1024);
constexpr auto kMaxUpgradeResponse = 8 * 1024;
constexpr auto kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr auto kSecureDefaultPort = 443;

class SocketThread final {
public:
	SocketThread();
	~SocketThread();

	[[nodiscard]] static std::shared_ptr<SocketThread> Acquire();
	[[nodiscard]] QObject *dispatcher();

private:
	QThread _thread;
	QObject *_dispatcher = nullptr;

};

SocketThread::SocketThread() : _dispatcher(new QObject()) {
	_dispatcher->moveToThread(&_thread);
	QObject::connect(
		&_thread,
		&QThread::finished,
		_dispatcher,
		&QObject::deleteLater);
	_thread.setObjectName(u"WebSocket"_q);
	_thread.start();
}

SocketThread::~SocketThread() {
	_thread.quit();
	_thread.wait();
}

std::shared_ptr<SocketThread> SocketThread::Acquire() {
	static auto mutex = std::mutex();
	static auto shared = std::weak_ptr<SocketThread>();
	const auto lock = std::lock_guard(mutex);
	auto result = shared.lock();
	if (!result) {
		result = std::make_shared<SocketThread>();
		shared = result;
	}
	return result;
}

QObject *SocketThread::dispatcher() {
	return _dispatcher;
}

} // namespace

class WebSocketClient::Worker final : public QObject {
public:
	Worker(WebSocketClient *owner, MTP::NetworkSnapshot route);

	void connectTo(
		const QString &host,
		int port,
		const QString &requestTarget,
		const QString &subprotocol);
	void sendText(const QByteArray &message);
	void sendBinary(const QByteArray &message);
	void close();

private:
	void notify(Event event, QByteArray payload = {});
	void onSslConnected();
	void onReadyRead();
	void sendUpgradeRequest();
	[[nodiscard]] bool readUpgradeResponse();
	[[nodiscard]] bool parseFrames();
	void writeFrame(const QByteArray &payload, quint8 opcode);
	void fail();

	enum class State {
		Idle,
		Connecting,
		WaitingUpgrade,
		Connected,
		Closed,
	};

	WebSocketClient *const _owner = nullptr;
	const MTP::NetworkSnapshot _route;
	QSslSocket _socket;
	QString _host;
	QString _target;
	QString _subprotocol;
	QByteArray _key;
	QByteArray _incoming;
	State _state = State::Idle;
	int _port = 0;

};

struct WebSocketClient::Private {
	Private(WebSocketClient *owner, MTP::NetworkSnapshot route);
	~Private();

	std::shared_ptr<SocketThread> thread = SocketThread::Acquire();
	Worker *worker = nullptr;
};

WebSocketClient::Worker::Worker(
		WebSocketClient *owner,
		MTP::NetworkSnapshot route)
: _owner(owner)
, _route(std::move(route)) {
	connect(&_socket, &QSslSocket::encrypted, this, [this] {
		onSslConnected();
	});
	connect(&_socket, &QSslSocket::readyRead, this, [this] {
		onReadyRead();
	});
	connect(&_socket, &QSslSocket::disconnected, this, [this] {
		fail();
	});
	connect(
		&_socket,
		&QAbstractSocket::errorOccurred,
		this,
		[this](QAbstractSocket::SocketError) { fail(); });
}

WebSocketClient::Private::Private(
		WebSocketClient *owner,
		MTP::NetworkSnapshot route) {
	QMetaObject::invokeMethod(thread->dispatcher(), [&] {
		worker = new Worker(owner, std::move(route));
	}, Qt::BlockingQueuedConnection);
}

WebSocketClient::Private::~Private() {
	QMetaObject::invokeMethod(thread->dispatcher(), [&] {
		worker->close();
		delete worker;
	}, Qt::BlockingQueuedConnection);
}

void WebSocketClient::Worker::notify(Event event, QByteArray payload) {
	QMetaObject::invokeMethod(_owner, [
		owner = _owner,
		event,
		payload = std::move(payload)
	] {
		owner->dispatch(event, payload);
	}, Qt::QueuedConnection);
}

void WebSocketClient::Worker::connectTo(
		const QString &host,
		int port,
		const QString &requestTarget,
		const QString &subprotocol) {
	if (_state != State::Idle) {
		return;
	}
	const auto proxy = _route.httpProxy();
	if (!proxy) {
		fail();
		return;
	}
	_host = host;
	_port = port;
	_target = requestTarget;
	_subprotocol = subprotocol;
	_state = State::Connecting;
	_socket.setProxy(*proxy);
	_socket.connectToHostEncrypted(host, quint16(port));
}

void WebSocketClient::Worker::onSslConnected() {
	if (!_route.current()) {
		fail();
		return;
	}
	if (_state != State::Connecting) {
		return;
	}
	_state = State::WaitingUpgrade;
	sendUpgradeRequest();
}

void WebSocketClient::Worker::sendUpgradeRequest() {
	auto keyBytes = std::array<uint8_t, 16>();
	base::RandomFill(keyBytes.data(), keyBytes.size());
	_key = QByteArray(
		reinterpret_cast<const char*>(keyBytes.data()),
		int(keyBytes.size())).toBase64();

	const auto host = (_port == kSecureDefaultPort)
		? _host
		: (_host + u":"_q + QString::number(_port));
	auto request = u"GET "_q + _target + u" HTTP/1.1\r\n"_q
		+ u"Host: "_q + host + u"\r\n"_q
		+ u"Upgrade: websocket\r\n"_q
		+ u"Connection: Upgrade\r\n"_q
		+ u"Sec-WebSocket-Key: "_q + QString::fromLatin1(_key) + u"\r\n"_q
		+ u"Sec-WebSocket-Version: 13\r\n"_q;
	if (!_subprotocol.isEmpty()) {
		request += u"Sec-WebSocket-Protocol: "_q + _subprotocol + u"\r\n"_q;
	}
	request += u"\r\n"_q;
	_socket.write(request.toLatin1());
}

bool WebSocketClient::Worker::readUpgradeResponse() {
	const auto end = _incoming.indexOf("\r\n\r\n");
	if (end < 0) {
		return (_incoming.size() < kMaxUpgradeResponse);
	}
	const auto head = _incoming.left(end);
	const auto lines = head.split('\n');
	const auto status = lines.isEmpty()
		? QByteArray()
		: lines.front().trimmed();
	if (!status.contains(" 101")) {
		return false;
	}
	const auto accept = _key + kAcceptGuid;
	const auto expected = QCryptographicHash::hash(
		accept,
		QCryptographicHash::Sha1).toBase64();
	auto accepted = false;
	for (const auto &line : lines) {
		const auto colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		} else if (line.left(colon).trimmed().toLower()
			!= "sec-websocket-accept") {
			continue;
		}
		accepted = (line.mid(colon + 1).trimmed() == expected);
		break;
	}
	if (!accepted) {
		return false;
	}
	_incoming.remove(0, end + 4);
	_state = State::Connected;
	notify(Event::Connected);
	return true;
}

void WebSocketClient::Worker::onReadyRead() {
	if (!_route.current()) {
		fail();
		return;
	}
	if (_state == State::WaitingUpgrade) {
		_incoming.append(_socket.readAll());
		if (!readUpgradeResponse()) {
			fail();
			return;
		} else if (_state != State::Connected) {
			return;
		}
	} else if (_state == State::Connected) {
		_incoming.append(_socket.readAll());
	} else {
		return;
	}
	if (!parseFrames()) {
		fail();
	}
}

bool WebSocketClient::Worker::parseFrames() {
	auto offset = 0;
	const auto size = _incoming.size();
	const auto data = reinterpret_cast<const uchar*>(_incoming.constData());
	while (_state == State::Connected) {
		if (size - offset < 2) {
			break;
		}
		const auto first = data[offset];
		const auto second = data[offset + 1];
		const auto opcode = quint8(first & 0x0F);
		const auto masked = ((second & 0x80) != 0);
		auto length = quint64(second & 0x7F);
		auto header = 2;
		if (length == 126) {
			if (size - offset < 4) {
				break;
			}
			length = (quint64(data[offset + 2]) << 8)
				| quint64(data[offset + 3]);
			header = 4;
		} else if (length == 127) {
			if (size - offset < 10) {
				break;
			}
			length = 0;
			for (auto i = 0; i != 8; ++i) {
				length = (length << 8) | quint64(data[offset + 2 + i]);
			}
			header = 10;
		}
		if (length > kMaxFramePayload) {
			return false;
		}
		if (masked) {
			header += 4;
		}
		if (quint64(size - offset) < quint64(header) + length) {
			break;
		}
		auto payload = QByteArray(
			_incoming.constData() + offset + header,
			int(length));
		if (masked) {
			const auto mask = data + offset + header - 4;
			for (auto i = 0; i != int(length); ++i) {
				payload[i] = char(payload[i] ^ char(mask[i % 4]));
			}
		}
		offset += header + int(length);
		notify(Event::Activity);

		switch (opcode) {
		case kOpcodeContinuation:
		case kOpcodeBinary:
			if (!payload.isEmpty()) {
				notify(Event::Binary, std::move(payload));
			}
			break;
		case kOpcodeText:
			if (!payload.isEmpty()) {
				notify(Event::Text, std::move(payload));
			}
			break;
		case kOpcodeClose:
			return false;
		case kOpcodePing:
			writeFrame(payload, kOpcodePong);
			break;
		case kOpcodePong:
			break;
		default:
			return false;
		}
	}
	if (offset > 0) {
		_incoming.remove(0, offset);
	}
	return true;
}

void WebSocketClient::Worker::writeFrame(
		const QByteArray &payload,
		quint8 opcode) {
	if (!_route.current()) {
		fail();
		return;
	}
	const auto length = quint64(payload.size());
	auto frame = QByteArray();
	frame.reserve(int(length) + 14);
	frame.append(char(0x80 | opcode));
	if (length < 126) {
		frame.append(char(0x80 | char(length)));
	} else if (length <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((length >> 8) & 0xFF));
		frame.append(char(length & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (auto i = 7; i >= 0; --i) {
			frame.append(char((length >> (i * 8)) & 0xFF));
		}
	}
	auto maskBytes = std::array<uint8_t, 4>();
	base::RandomFill(maskBytes.data(), maskBytes.size());
	const auto mask = reinterpret_cast<const char*>(maskBytes.data());
	frame.append(mask, 4);

	const auto from = payload.constData();
	for (auto i = 0; i != int(length); ++i) {
		frame.append(char(from[i] ^ mask[i % 4]));
	}
	_socket.write(frame);
}

void WebSocketClient::Worker::sendText(const QByteArray &message) {
	if (_state != State::Connected) {
		return;
	}
	writeFrame(message, kOpcodeText);
}

void WebSocketClient::Worker::sendBinary(const QByteArray &message) {
	if (_state != State::Connected) {
		return;
	}
	writeFrame(message, kOpcodeBinary);
}

void WebSocketClient::Worker::close() {
	if (_state == State::Connected && _route.current()) {
		writeFrame(QByteArray(), kOpcodeClose);
	}
	_state = State::Closed;
	_socket.abort();
}

void WebSocketClient::Worker::fail() {
	if (_state == State::Closed) {
		return;
	}
	_state = State::Closed;
	_socket.abort();
	notify(Event::Closed);
}

WebSocketClient::WebSocketClient(QObject *parent) : QObject(parent) {
}

WebSocketClient::WebSocketClient(
		MTP::NetworkContext &network,
		QObject *parent)
: WebSocketClient(parent) {
	_route = network.snapshot();
	network.changes() | rpl::on_next([this] { fail(); }, _lifetime);
}

WebSocketClient::~WebSocketClient() {
	_lifetime.destroy();
	close();
}

void WebSocketClient::connectTo(
		const QString &host,
		int port,
		const QString &requestTarget,
		const QString &subprotocol) {
	Expects(QThread::currentThread() == thread());
	if (_started || _closed) {
		return;
	}
	_started = true;
	if (!_route.httpProxy()) {
		fail();
		return;
	}
	_private = std::make_unique<Private>(this, _route);
	const auto worker = _private->worker;
	QMetaObject::invokeMethod(worker, [=] {
		worker->connectTo(host, port, requestTarget, subprotocol);
	}, Qt::QueuedConnection);
}

void WebSocketClient::sendText(const QByteArray &message) {
	Expects(QThread::currentThread() == thread());
	if (!_private || _closed) {
		return;
	}
	const auto worker = _private->worker;
	QMetaObject::invokeMethod(worker, [=] {
		worker->sendText(message);
	}, Qt::QueuedConnection);
}

void WebSocketClient::sendBinary(const QByteArray &message) {
	Expects(QThread::currentThread() == thread());
	if (!_private || _closed) {
		return;
	}
	const auto worker = _private->worker;
	QMetaObject::invokeMethod(worker, [=] {
		worker->sendBinary(message);
	}, Qt::QueuedConnection);
}

void WebSocketClient::close() {
	Expects(QThread::currentThread() == thread());
	_closed = true;
	_private.reset();
}

void WebSocketClient::fail() {
	if (_closed) {
		return;
	}
	close();
	if (const auto handler = onClosed) {
		handler();
	}
}

void WebSocketClient::dispatch(Event event, QByteArray payload) {
	if (_closed) {
		return;
	} else if (event == Event::Closed || !_route.current()) {
		fail();
		return;
	}
	switch (event) {
	case Event::Connected:
		if (const auto handler = onConnected) {
			handler();
		}
		break;
	case Event::Text:
		if (const auto handler = onText) {
			handler(std::move(payload));
		}
		break;
	case Event::Binary:
		if (const auto handler = onBinary) {
			handler(std::move(payload));
		}
		break;
	case Event::Activity:
		if (const auto handler = onActivity) {
			handler();
		}
		break;
	case Event::Closed:
		Unexpected("Closed WebSocket event.");
	}
}

} // namespace Core
