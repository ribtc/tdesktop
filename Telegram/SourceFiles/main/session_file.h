#pragma once

#include <QtCore/QString>
#include <array>
#include <variant>

namespace Main {

struct SessionCredentials {
	int dcId = 0;
	std::array<unsigned char, 256> authKey = {};
};

enum class SessionFileError {
	Unreadable,
	Unsupported,
	Invalid,
	Unauthorized,
};

using SessionFileResult = std::variant<SessionCredentials, SessionFileError>;

[[nodiscard]] SessionFileResult ReadSessionFile(const QString &path);

} // namespace Main
