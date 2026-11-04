#pragma once

#include <QtCore/QString>
#include <array>
#include <optional>
#include <variant>

namespace Main {

struct SessionProxy {
	QString type;
	QString host;
	int port = 0;
	QString username;
	QString password;
	bool preferIPv6 = false;
};

struct SessionCredentials {
	int dcId = 0;
	bool proxySpecified = false;
	std::optional<SessionProxy> proxy;
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
