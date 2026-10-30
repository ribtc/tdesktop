/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Core {

[[nodiscard]] inline QString AccountWebViewError() {
	return u"This web content is unavailable because the embedded browser "
		"cannot use an isolated account proxy."_q;
}

[[nodiscard]] inline QString AccountSystemNetworkError() {
	return u"This system service is unavailable because its network requests "
		"cannot use an isolated account proxy."_q;
}

} // namespace Core
