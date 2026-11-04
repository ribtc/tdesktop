/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lang/lang_keys.h"

namespace Core {

[[nodiscard]] inline QString AccountWebViewError() {
	return Lang::EarlyValue(tr::lng_account_web_blocked.base);
}

[[nodiscard]] inline QString AccountSystemNetworkError() {
	return Lang::EarlyValue(tr::lng_account_system_blocked.base);
}

} // namespace Core
