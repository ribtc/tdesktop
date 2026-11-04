/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/network_context.h"

namespace Calls {

// RTC engines must prove proxy-only transport before this policy permits them.
enum class MediaTransport {
	Rtc,
	MtpBroadcast,
};

[[nodiscard]] inline bool MediaAllowed(
		const MTP::NetworkSnapshot &network,
		MediaTransport transport) {
	return network.allowed()
		&& network.state == MTP::NetworkState::Ready
		&& transport == MediaTransport::MtpBroadcast;
}

[[nodiscard]] bool CheckMediaAllowed(
	const MTP::NetworkSnapshot &network,
	MediaTransport transport);

} // namespace Calls
