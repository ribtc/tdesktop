/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/network_context.h"

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
