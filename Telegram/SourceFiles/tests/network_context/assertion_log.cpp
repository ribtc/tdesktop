/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <cstdio>

namespace base::assertion {

void log(const char *message, const char *file, int line) {
	std::fprintf(stderr, "%s:%d: %s\n", file, line, message);
}

} // namespace base::assertion
