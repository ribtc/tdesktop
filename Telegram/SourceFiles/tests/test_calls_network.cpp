/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_network.h"
#include "mtproto/network_context.h"

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

int main() {
	using Transport = Calls::MediaTransport;
	const auto allowed = [](const MTP::NetworkSnapshot &network) {
		return Calls::MediaAllowed(network, Transport::MtpBroadcast);
	};
	auto account = MTP::NetworkContext();
	Require(!allowed(account.snapshot()), "Unbound accounts cannot play a stream.");
	auto proxy = MTP::ProxyData();
	proxy.type = MTP::ProxyData::Type::Socks5;
	proxy.host = u"127.0.0.1"_q;
	proxy.port = 1080;
	const auto checking = account.bind(u"calls-account"_q, proxy, false);
	Require(!allowed(checking), "Checking accounts cannot play a stream.");
	Require(account.completeValidation(checking, true), "SOCKS5 route is ready.");
	const auto playing = account.snapshot();
	Require(!Calls::MediaAllowed(playing, Transport::Rtc),
		"Unverified RTC engines cannot start on a SOCKS5 account.");
	Require(allowed(playing), "SOCKS5 accounts retain MTP broadcast playback.");

	auto other = MTP::NetworkContext();
	proxy.type = MTP::ProxyData::Type::Mtproto;
	proxy.password = u"00112233445566778899aabbccddeeff"_q;
	const auto otherCheck = other.bind(u"other-account"_q, proxy, false);
	Require(other.completeValidation(otherCheck, true), "MTProto route is ready.");
	const auto otherPlaying = other.snapshot();
	Require(!Calls::MediaAllowed(otherPlaying, Transport::Rtc),
		"Unverified RTC engines cannot start on an MTProto account.");
	Require(allowed(otherPlaying), "MTProto accounts retain MTP broadcast playback.");

	account.block();
	Require(!allowed(playing), "Revoked streams cannot consume delayed media.");
	Require(!allowed(account.snapshot()), "Failed accounts cannot rejoin a stream.");
	Require(allowed(otherPlaying), "Revoking one account preserves the other stream.");
	const auto replacement = account.bind(u"replacement"_q, proxy, false);
	Require(account.completeValidation(replacement, true), "Replacement route is ready.");
	Require(!allowed(playing), "Rebinding cannot revive the old media controller.");
	Require(allowed(account.snapshot()), "Joining again uses the replacement route.");
	Require(allowed(otherPlaying), "Rebinding leaves the other stream untouched.");

	auto destroyed = MTP::NetworkSnapshot();
	{
		auto temporary = MTP::NetworkContext();
		const auto check = temporary.bind(u"temporary"_q, proxy, false);
		Require(temporary.completeValidation(check, true), "Temporary route is ready.");
		destroyed = temporary.snapshot();
		Require(allowed(destroyed), "A live account can play its stream.");
	}
	Require(!allowed(destroyed), "Destroying the account revokes delayed media.");
	auto legacy = MTP::NetworkContext(MTP::NetworkContext::Mode::Legacy);
	Require(!allowed(legacy.snapshot()), "Legacy global routes cannot authorize account media.");
}
