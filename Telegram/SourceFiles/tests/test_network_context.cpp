/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/proxy_library.h"
#include "mtproto/http_transport.h"
#include "mtproto/network_context.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

namespace {

void Require(bool value, const char *message) {
	if (!value) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

} // namespace

void TestHttpRouting() {
	auto server = QTcpServer();
	Require(server.listen(QHostAddress::LocalHost), "The local proxy starts.");
	auto unrelated = QTcpServer();
	Require(unrelated.listen(QHostAddress::LocalHost), "The unrelated proxy starts.");
	QNetworkProxy::setApplicationProxy(QNetworkProxy(
		QNetworkProxy::Socks5Proxy,
		u"127.0.0.1"_q,
		unrelated.serverPort()));
	auto account = MTP::NetworkContext();
	auto proxy = MTP::ProxyData();
	proxy.type = MTP::ProxyData::Type::Socks5;
	proxy.host = u"127.0.0.1"_q;
	proxy.port = server.serverPort();
	const auto check = account.bind(u"http-account"_q, proxy, false);
	Require(account.completeValidation(check, true), "The test route is ready.");
	auto transport = MTP::HttpTransport(account);
	auto target = QByteArray();
	auto hold = false;
	auto pendingSeen = false;
	QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
		const auto socket = server.nextPendingConnection();
		const auto stage = std::make_shared<int>(0);
		const auto buffer = std::make_shared<QByteArray>();
		QObject::connect(socket, &QTcpSocket::readyRead, socket, [=,
				&target,
				&hold,
				&pendingSeen] {
			buffer->append(socket->readAll());
			if (*stage == 0 && buffer->size() >= 3) {
				buffer->clear();
				socket->write(QByteArray::fromHex("0500"));
				*stage = 1;
			} else if (*stage == 1 && buffer->size() >= 7) {
				Require(buffer->at(3) == 3,
					"The destination reaches SOCKS5 as a hostname.");
				target = buffer->mid(5, uchar(buffer->at(4)));
				buffer->clear();
				socket->write(QByteArray::fromHex("050000017f0000010050"));
				*stage = 2;
			} else if (*stage == 2 && buffer->contains("\r\n\r\n")) {
				if (hold) {
					pendingSeen = true;
				} else {
					socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
						"Connection: close\r\n\r\nOK");
					socket->disconnectFromHost();
				}
				*stage = 3;
			}
		});
	});
	const auto reply = transport.get(QNetworkRequest(
		QUrl(u"http://account-route.invalid/file"_q)));
	auto timer = QElapsedTimer();
	timer.start();
	while (!reply->isFinished() && timer.elapsed() < 3000) {
		QCoreApplication::processEvents();
	}
	Require(reply->isFinished() && reply->error() == QNetworkReply::NoError
		&& reply->readAll() == "OK" && target == "account-route.invalid",
		"HTTP reaches the bound proxy with the destination hostname intact.");
	hold = true;
	const auto pending = transport.get(QNetworkRequest(
		QUrl(u"http://account-route.invalid/pending"_q)));
	timer.restart();
	while (!pendingSeen && timer.elapsed() < 3000) {
		QCoreApplication::processEvents();
	}
	Require(pendingSeen, "The pending request has reached its proxy.");
	account.block();
	Require(pending->isFinished()
		&& pending->error() == QNetworkReply::OperationCanceledError,
		"Blocking the account synchronously aborts its active HTTP reply.");
	const auto blocked = transport.get(QNetworkRequest(
		QUrl(u"http://account-route.invalid/blocked"_q)));
	Require(blocked->isFinished()
		&& blocked->error() == QNetworkReply::ProxyNotFoundError,
		"A revoked HTTP transport cannot start another request.");
	auto temporary = std::make_unique<MTP::NetworkContext>();
	const auto temporaryCheck = temporary->bind(u"temporary"_q, proxy, false);
	Require(temporary->completeValidation(temporaryCheck, true),
		"A temporary account has a valid route.");
	auto temporaryHttp = MTP::HttpTransport(*temporary);
	const auto temporaryReply = temporaryHttp.get(QNetworkRequest(
		QUrl(u"http://account-route.invalid/pending"_q)));
	temporary = nullptr;
	Require(temporaryReply->isFinished()
		&& temporaryReply->error() == QNetworkReply::OperationCanceledError,
		"Destroying an account closes its outstanding HTTP request.");
	auto direct = QTcpServer();
	direct.setProxy(QNetworkProxy::NoProxy);
	Require(direct.listen(QHostAddress::LocalHost), "The direct observer starts.");
	auto unbound = MTP::NetworkContext();
	auto denied = MTP::HttpTransport(unbound);
	const auto deniedReply = denied.get(QNetworkRequest(QUrl(
		u"http://127.0.0.1:%1/private"_q.arg(direct.serverPort()))));
	QCoreApplication::processEvents();
	Require(deniedReply->error() == QNetworkReply::ProxyNotFoundError
		&& !direct.hasPendingConnections() && !unrelated.hasPendingConnections(),
		"An unbound request reaches neither its direct target nor a global proxy.");
	QNetworkProxy::setApplicationProxy(QNetworkProxy::DefaultProxy);
}

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	TestHttpRouting();
	auto library = Core::ProxyLibrary();
	auto savedProxy = MTP::ProxyData();
	savedProxy.type = MTP::ProxyData::Type::Socks5;
	savedProxy.host = u"192.0.2.9"_q;
	savedProxy.port = 1080;
	const auto id = library.add(savedProxy, u"Work"_q, true);
	Require(!id.isEmpty(), "A new proxy has a stable identifier.");
	auto restored = Core::ProxyLibrary();
	Require(restored.load(library.serialize()), "Saved proxies can be restored.");
	const auto entry = restored.find(id);
	Require(entry && entry->proxy == savedProxy && entry->tryIPv6,
		"Restoring preserves the proxy identifier and route.");
	auto connectionChanges = 0;
	auto observer = rpl::lifetime();
	restored.changes() | rpl::on_next([&](Core::ProxyLibraryChange change) {
		if (change.connection) {
			++connectionChanges;
		}
	}, observer);
	auto annotated = *entry;
	annotated.notes = u"Updated note"_q;
	Require(restored.update(annotated) && connectionChanges == 0
		&& restored.find(id)->revision == annotated.revision,
		"Editing a note preserves the route version and never requests reconnect.");
	auto hostname = savedProxy;
	hostname.host = u"proxy.example.com"_q;
	Require(library.add(hostname).isEmpty(),
		"New proxies cannot trigger local name resolution.");
	auto migrated = Core::ProxyLibrary();
	migrated.importLegacy({ hostname }, savedProxy, false);
	Require(migrated.entries().size() == 2,
		"Unsupported legacy entries and selected proxies are preserved.");
	const auto legacyId = migrated.entries().front().id;
	migrated.importLegacy({ savedProxy }, savedProxy, false);
	Require(migrated.entries().size() == 2
		&& migrated.entries().front().id == legacyId,
		"Migration runs only once and preserves identifiers.");
	auto corrupt = Core::ProxyLibrary();
	const auto broken = QByteArray("{\"version\":99,\"entries\":[]}");
	Require(!corrupt.load(broken) && !corrupt.writable()
		&& corrupt.serialize() == broken && corrupt.add(savedProxy).isEmpty(),
		"Unknown libraries remain intact and cannot be overwritten.");
	auto edit = *restored.find(id);
	auto stale = edit;
	edit.proxy.host = u"192.0.2.10"_q;
	Require(restored.update(edit), "An existing proxy can change its route.");
	Require(restored.find(id)->id == id && restored.find(id)->revision == 2,
		"Changing a route preserves its identity and advances its revision.");
	Require(!restored.update(stale),
		"An old editor cannot overwrite a newer route.");
	auto isolated = Core::ProxyLibrary();
	const auto goodId = isolated.add(savedProxy);
	const auto badId = isolated.add(savedProxy);
	auto malformedRoot = QJsonDocument::fromJson(isolated.serialize()).object();
	auto malformedEntries = malformedRoot.value("entries").toArray();
	auto malformedEntry = malformedEntries[1].toObject();
	malformedEntry.insert("port", "1080");
	malformedEntries[1] = malformedEntry;
	malformedRoot.insert("entries", malformedEntries);
	Require(corrupt.load(QJsonDocument(malformedRoot).toJson())
		&& corrupt.find(goodId) && !corrupt.find(badId),
		"One malformed record cannot block an unrelated valid proxy.");
	Require(corrupt.remove(goodId), "Valid records remain editable.");
	Require(restored.load(corrupt.serialize())
		&& !restored.find(badId)
		&& QJsonDocument::fromJson(restored.serialize()).object()
			.value("entries").toArray().contains(malformedEntry),
		"Quarantined records survive valid edits and another restart intact.");
	malformedEntries[1] = malformedEntries[0];
	malformedRoot.insert("entries", malformedEntries);
	Require(corrupt.load(QJsonDocument(malformedRoot).toJson())
		&& !corrupt.find(goodId) && corrupt.entries().empty(),
		"Duplicate identities quarantine every candidate, never pick a route.");
	for (const auto &field : { "type", "port", "revision" }) {
		auto invalidEntry = malformedEntry;
		invalidEntry.insert("port", 1080);
		invalidEntry.insert(field, field == QString("revision")
			? QJsonValue("0") : QJsonValue(999999));
		malformedEntries[1] = invalidEntry;
		malformedRoot.insert("entries", malformedEntries);
		Require(corrupt.load(QJsonDocument(malformedRoot).toJson())
			&& corrupt.find(goodId) && !corrupt.find(badId),
			"Invalid enums, ports and revisions cannot escape quarantine.");
	}
	Require(corrupt.identifiers().size() == 2,
		"Destructive previews include identities of quarantined records.");
	Require(corrupt.clear() && restored.load(corrupt.serialize())
		&& restored.identifiers().empty()
		&& QJsonDocument::fromJson(restored.serialize()).object()
			.value("entries").toArray().empty(),
		"An explicit library clear removes valid and quarantined records.");
	auto account = MTP::NetworkContext();
	Require(!account.snapshot().allowed(),
		"An unbound account must not connect.");
	auto other = MTP::NetworkContext();
	auto proxy = MTP::ProxyData();
	proxy.type = MTP::ProxyData::Type::Socks5;
	proxy.host = u"192.0.2.1"_q;
	proxy.port = 1080;
	const auto pending = account.bind(u"proxy-a"_q, proxy, false);
	Require(!pending.allowed(),
		"Checking must not allow business connections.");
	Require(!other.completeValidation(pending, true),
		"Another account cannot accept a check.");
	Require(account.completeValidation(pending, true),
		"The bound proxy check must be accepted.");
	Require(account.snapshot().allowed(),
		"A successful check must allow connections.");
	const auto httpProxy = account.snapshot().httpProxy();
	Require(httpProxy
		&& httpProxy->type() == QNetworkProxy::Socks5Proxy
		&& httpProxy->hostName() == u"192.0.2.1"_q
		&& httpProxy->port() == 1080
		&& httpProxy->capabilities().testFlag(
			QNetworkProxy::HostNameLookupCapability),
		"HTTP uses the account SOCKS5 route and proxy-side DNS.");
	Require(account.snapshot().proxy.host == u"192.0.2.1"_q,
		"Connections must use the bound proxy.");
	Require(!other.snapshot().allowed(),
		"Checking one account must not connect another.");
	auto secondProxy = proxy;
	secondProxy.host = u"192.0.2.2"_q;
	const auto secondCheck = other.bind(u"proxy-b"_q, secondProxy, true);
	Require(other.completeValidation(secondCheck, true),
		"The second account can connect independently.");
	const auto connected = account.snapshot();
	auto revoked = std::promise<void>();
	auto notice = revoked.get_future();
	auto worker = std::thread([connected, notice = std::move(notice)]() mutable {
		notice.wait();
		Require(!connected.allowed(),
			"A connection worker must observe revocation.");
	});
	account.block();
	revoked.set_value();
	worker.join();
	Require(other.snapshot().allowed(),
		"Blocking one account must leave the other connected.");
	Require(other.snapshot().proxy.host == u"192.0.2.2"_q,
		"The other account must retain its own route.");
	Require(!connected.allowed() && !connected.httpProxy(),
		"Blocking must immediately revoke an existing connection.");
	Require(!account.completeValidation(pending, true),
		"Late checks cannot undo blocking.");
	Require(!account.snapshot().allowed(),
		"A late success must leave the account blocked.");

	auto legacy = MTP::NetworkContext(MTP::NetworkContext::Mode::Legacy);
	legacy.applyLegacy(proxy, true, true);
	Require(legacy.snapshot().allowed(),
		"Legacy proxy connections remain available.");
	Require(legacy.snapshot().tryIPv6,
		"Legacy address preference must be retained.");
	legacy.applyLegacy(proxy, false, false);
	Require(legacy.snapshot().proxy.type == MTP::ProxyData::Type::None,
		"Disabling the legacy proxy must restore the direct configuration.");
	const auto strict = legacy.bind(u"strict"_q, proxy, false);
	legacy.applyLegacy({}, false, true);
	Require(!legacy.snapshot().allowed(),
		"Global changes must not release an isolated account.");
	Require(legacy.completeValidation(strict, true),
		"Global changes must not replace a bound proxy.");
	Require(legacy.snapshot().proxy.host == u"192.0.2.1"_q,
		"Global changes must not change an isolated account's route.");
	const auto invalid = account.bind(u"missing-proxy"_q, {}, false);
	Require(!account.completeValidation(invalid, true),
		"An absent proxy cannot pass validation.");
	Require(!account.snapshot().allowed(),
		"An absent proxy cannot enable direct fallback.");
	Require(!account.updateResolved(pending, { u"192.0.2.55"_q }, 1000),
		"A late DNS result cannot change a replacement binding.");
	auto resolving = MTP::NetworkContext(MTP::NetworkContext::Mode::Legacy);
	resolving.applyLegacy(proxy, true, false);
	const auto legacyRoute = resolving.snapshot();
	Require(resolving.updateResolved(legacyRoute, { u"192.0.2.56"_q }, 1000),
		"Resolution for a current route can update its address cache.");
	Require(legacyRoute.proxy.resolvedIPs.empty(),
		"Existing route snapshots remain unchanged.");
	auto mtproto = proxy;
	mtproto.type = MTP::ProxyData::Type::Mtproto;
	mtproto.password = QString(32, QChar('1'));
	const auto mtprotoCheck = account.bind(u"mtp"_q, mtproto, false);
	Require(account.completeValidation(mtprotoCheck, true)
		&& account.snapshot().allowed() && !account.snapshot().httpProxy(),
		"MTProto remains available without granting a direct HTTP fallback.");
	return 0;
}
