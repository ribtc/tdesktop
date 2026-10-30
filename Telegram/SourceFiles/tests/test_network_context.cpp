/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/network_context.h"
#include "core/proxy_library.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>

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

int main() {
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
	auto malformedRoot = QJsonDocument::fromJson(library.serialize()).object();
	auto malformedEntries = malformedRoot.value("entries").toArray();
	auto malformedEntry = malformedEntries.first().toObject();
	malformedEntry.insert("port", "1080");
	malformedEntries[0] = malformedEntry;
	malformedRoot.insert("entries", malformedEntries);
	const auto malformedBytes = QJsonDocument(malformedRoot).toJson();
	Require(!corrupt.load(malformedBytes) && corrupt.serialize() == malformedBytes,
		"Malformed records are preserved without silently rewriting fields.");
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
	Require(!connected.allowed(),
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
	return 0;
}
