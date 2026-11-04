/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/websocket_client.h"
#include "mtproto/network_context.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QNetworkProxy>
#include <cstdio>
#include <cstdlib>

namespace {

void Require(bool value, const char *message) {
	if (!value) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

} // namespace

int main(int argc, char *argv[]) {
	auto app = QCoreApplication(argc, argv);
	auto unrelatedProxy = QTcpServer();
	Require(unrelatedProxy.listen(QHostAddress::LocalHost), "The other proxy starts.");
	QNetworkProxy::setApplicationProxy(QNetworkProxy(
		QNetworkProxy::Socks5Proxy, u"127.0.0.1"_q,
		unrelatedProxy.serverPort()));
	auto destination = QTcpServer();
	Require(destination.listen(QHostAddress::LocalHost), "Local observer starts.");
	auto socket = Core::WebSocketClient();
	auto closed = false;
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	socket.onClosed = [&] { closed = true; loop.quit(); };
	QObject::connect(&destination, &QTcpServer::newConnection, &loop, [&] {
		loop.quit();
	});
	timeout.start(2000);
	socket.connectTo(u"127.0.0.1"_q, destination.serverPort(), u"/"_q, {});
	if (!closed) {
		loop.exec();
	}
	timeout.stop();
	Require(!destination.hasPendingConnections(),
		"An unbound WebSocket must not connect directly.");
	Require(closed, "An unbound WebSocket reports that the operation failed.");
	auto proxy = QTcpServer();
	Require(proxy.listen(QHostAddress::LocalHost), "The SOCKS5 observer starts.");
	auto route = MTP::NetworkContext();
	auto data = MTP::ProxyData();
	data.type = MTP::ProxyData::Type::Socks5;
	data.host = u"127.0.0.1"_q;
	data.port = proxy.serverPort();
	const auto check = route.bind(u"account-a"_q, data, false);
	Require(route.completeValidation(check, true), "The fixture validates its route.");
	auto routed = Core::WebSocketClient(route);
	auto observed = QByteArray();
	auto greeted = false;
	auto connection = QPointer<QTcpSocket>();
	QObject::connect(&proxy, &QTcpServer::newConnection, &loop, [&] {
		connection = proxy.nextPendingConnection();
		QObject::connect(connection, &QTcpSocket::readyRead, &loop, [&] {
			observed += connection->readAll();
			if (!greeted && observed.size() >= 3) {
				Require(observed.at(0) == char(5), "The proxy receives SOCKS5.");
				greeted = true;
				observed.clear();
				connection->write(QByteArray::fromHex("0500"));
			} else if (greeted && observed.size() >= 7 + 14) {
				loop.quit();
			}
		});
	});
	routed.connectTo(u"target.invalid"_q, 443, u"/"_q, {});
	timeout.start(2000);
	loop.exec();
	timeout.stop();
	Require(observed == QByteArray::fromHex("050100030e")
		+ "target.invalid" + QByteArray::fromHex("01bb"),
		"The account proxy receives the target hostname for remote DNS.");
	auto disconnected = false;
	QObject::connect(connection, &QTcpSocket::disconnected, &loop, [&] {
		disconnected = true;
		loop.quit();
	});
	route.block();
	if (!disconnected) {
		timeout.start(2000);
		loop.exec();
	}
	timeout.stop();
	Require(disconnected, "Revoking the account closes its active proxy connection.");
	Require(!unrelatedProxy.hasPendingConnections(),
		"Account requests never use the process-wide proxy.");
	auto mtproto = MTP::NetworkContext();
	data.type = MTP::ProxyData::Type::Mtproto;
	data.password = QString(32, QChar('0'));
	const auto mtprotoCheck = mtproto.bind(u"account-b"_q, data, false);
	Require(mtproto.completeValidation(mtprotoCheck, true),
		"The fixture validates its MTProto binding.");
	auto unsupported = Core::WebSocketClient(mtproto);
	auto rejected = false;
	unsupported.onClosed = [&] { rejected = true; };
	unsupported.connectTo(u"127.0.0.1"_q, destination.serverPort(), u"/"_q, {});
	Require(rejected && !destination.hasPendingConnections()
		&& !proxy.hasPendingConnections() && !unrelatedProxy.hasPendingConnections(),
		"MTProto cannot silently become a direct or shared WebSocket route.");

}
