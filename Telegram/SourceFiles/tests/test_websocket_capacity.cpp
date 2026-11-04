#include "core/websocket_client.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QSslConfiguration>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

#ifdef Q_OS_MAC
#include <QtGui/QGuiApplication>
#endif // Q_OS_MAC

namespace {

void Require(bool value, const char *message) {
	if (!value) {
		std::fprintf(stderr, "%s\n", message);
		throw std::runtime_error(message);
	}
}

bool Wait(const std::function<bool()> &done) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!done() && timer.elapsed() < 5000) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return done();
}

class Descriptors final {
public:
	Descriptors();
	~Descriptors();

private:
	std::vector<int> _fds;

};

Descriptors::Descriptors() {
	auto limit = rlimit();
	Require(getrlimit(RLIMIT_NOFILE, &limit) == 0, "Read descriptor limit.");
	limit.rlim_cur = std::max<rlim_t>(limit.rlim_cur, 2048);
	Require(setrlimit(RLIMIT_NOFILE, &limit) == 0, "Raise descriptor limit.");
	do {
		const auto descriptor = open("/dev/null", O_RDONLY);
		Require(descriptor >= 0, "Reserve descriptors.");
		_fds.push_back(descriptor);
	} while (_fds.back() < 1100);
}

Descriptors::~Descriptors() {
	for (const auto descriptor : _fds) {
		close(descriptor);
	}
}

void Bind(MTP::NetworkContext &route, int port) {
	auto proxy = MTP::ProxyData();
	proxy.type = MTP::ProxyData::Type::Socks5;
	proxy.host = u"127.0.0.1"_q;
	proxy.port = port;
	const auto check = route.bind(u"fixture"_q, proxy, false);
	Require(route.completeValidation(check, true), "Establish fixture route.");
}

} // namespace

int main(int argc, char *argv[]) try {
#ifdef Q_OS_MAC
	auto app = QGuiApplication(argc, argv);
#else
	auto app = QCoreApplication(argc, argv);
#endif // Q_OS_MAC
	Require(argc == 3, "Pass Python and native server fixture paths.");
	auto directory = QTemporaryDir();
	const auto certificate = directory.filePath(u"certificate.pem"_q);
	const auto key = directory.filePath(u"key.pem"_q);
	auto identity = QProcess();
	identity.setStandardErrorFile(QProcess::nullDevice());
	identity.start(u"openssl"_q, {
		u"req"_q, u"-x509"_q, u"-newkey"_q, u"rsa:2048"_q,
		u"-nodes"_q, u"-days"_q, u"1"_q,
		u"-subj"_q, u"/CN=target.invalid"_q,
		u"-addext"_q, u"subjectAltName=DNS:target.invalid"_q,
		u"-keyout"_q, key, u"-out"_q, certificate,
	});
	Require(
		identity.waitForFinished() && identity.exitCode() == 0,
		"Create temporary TLS fixture identity.");
	auto ssl = QSslConfiguration::defaultConfiguration();
	// The temporary self-signed endpoint exercises I/O, not trust validation.
	ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
	QSslConfiguration::setDefaultConfiguration(ssl);
	auto server = QProcess();
	server.setProcessChannelMode(QProcess::ForwardedErrorChannel);
	server.start(QString::fromLocal8Bit(argv[1]), {
		QString::fromLocal8Bit(argv[2]), certificate, key,
	});
	Require(server.waitForStarted() && server.waitForReadyRead(), "Start native server.");
	const auto port = server.readLine().trimmed().toInt();
	Require(port > 0, "Read native server port.");
	auto otherServer = QProcess();
	otherServer.setProcessChannelMode(QProcess::ForwardedErrorChannel);
	otherServer.start(QString::fromLocal8Bit(argv[1]), {
		QString::fromLocal8Bit(argv[2]), certificate, key,
	});
	Require(
		otherServer.waitForStarted() && otherServer.waitForReadyRead(),
		"Start second account's native server.");
	const auto otherPort = otherServer.readLine().trimmed().toInt();
	Require(otherPort > 0 && otherPort != port, "Use distinct account proxies.");
	const auto descriptors = Descriptors();
	auto routeA = MTP::NetworkContext();
	auto routeB = MTP::NetworkContext();
	Bind(routeA, port);
	Bind(routeB, otherPort);
	auto a = std::make_unique<Core::WebSocketClient>(routeA);
	auto b = Core::WebSocketClient(routeB);
	auto aConnected = false;
	auto bConnected = false;
	auto aClosed = 0;
	auto bClosed = 0;
	auto aMessages = 0;
	auto bMessage = QByteArray();
	auto bTextMessages = 0;
	a->onConnected = [&] { aConnected = true; };
	b.onConnected = [&] { bConnected = true; };
	a->onClosed = [&] { ++aClosed; };
	b.onClosed = [&] { ++bClosed; };
	a->onText = [&](QByteArray) {
		Require(
			QThread::currentThread() == app.thread(),
			"Text callbacks run on the client thread.");
		++aMessages;
	};
	b.onBinary = [&](QByteArray message) {
		Require(
			QThread::currentThread() == app.thread(),
			"Binary callbacks run on the client thread.");
		bMessage = message;
	};
	b.onText = [&](QByteArray) { ++bTextMessages; };
	a->connectTo(u"target.invalid"_q, 443, u"/"_q, {});
	b.connectTo(u"target.invalid"_q, 443, u"/"_q, {});
	Require(
		Wait([&] { return aConnected && bConnected; }),
		"Two account WebSockets connect through TLS above descriptor 1100.");
	a->sendText("first");
	b.sendBinary("binary");
	Require(
		Wait([&] { return aMessages == 1 && bMessage == "binary"; }),
		"Both account WebSockets exchange frames above descriptor 1100.");
	a->sendText("stale");
	auto observed = server.readAllStandardOutput();
	while (!observed.contains("pong")) {
		Require(server.waitForReadyRead(5000), "Observe server's pending response.");
		observed += server.readAllStandardOutput();
	}
	routeA.block();
	Require(aClosed == 1, "Revocation notifies closure synchronously.");
	Require(
		Wait([&] {
			observed += server.readAllStandardOutput();
			return observed.contains("closed");
		}),
		"Revocation aborts the remote connection.");
	Require(aMessages == 1, "Revocation suppresses queued old-generation frames.");
	a.reset();
	bMessage.clear();
	b.sendBinary("survivor");
	Require(
		Wait([&] { return bMessage == "survivor"; }) && bClosed == 0,
		"Destroying account A leaves account B's worker and socket usable.");
	b.sendText("stale");
	observed = otherServer.readAllStandardOutput();
	while (!observed.contains("pong")) {
		Require(otherServer.waitForReadyRead(5000), "Observe worker's control response.");
		observed += otherServer.readAllStandardOutput();
	}
	b.close();
	Require(
		Wait([&] {
			observed += otherServer.readAllStandardOutput();
			return observed.contains("closed");
		}),
		"Explicit close aborts the remote socket.");
	Require(
		bTextMessages == 0 && bClosed == 0,
		"Explicit close suppresses queued frames without failure notification.");
	auto selfDeleting = std::make_unique<Core::WebSocketClient>(routeB);
	selfDeleting->onConnected = [&] { selfDeleting.reset(); };
	selfDeleting->connectTo(u"target.invalid"_q, 443, u"/"_q, {});
	Require(
		Wait([&] { return !selfDeleting; }),
		"A callback may destroy its client after the shared worker restarts.");
	otherServer.kill();
	Require(otherServer.waitForFinished(), "Stop second native fixture.");
	server.kill();
	Require(server.waitForFinished(), "Stop native fixture.");
} catch (const std::exception &) {
	return 1;
}
