#include "core/proxy_import.h"

#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>

namespace Core {
namespace {

QByteArray Identity(const MTP::ProxyData &proxy) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	const auto address = QHostAddress(proxy.host);
	stream << int(proxy.type) << address.toString() << proxy.port;
	if (proxy.type == MTP::ProxyData::Type::Mtproto && proxy.valid()) {
		const auto secret = proxy.secretFromMtprotoPassword();
		stream << QByteArray(reinterpret_cast<const char*>(secret.data()), secret.size());
	} else {
		stream << proxy.user << proxy.password;
	}
	return result;
}

ProxyImportError ParseLine(const QString &line, MTP::ProxyData &proxy) {
	using Error = ProxyImportError;
	using Type = MTP::ProxyData::Type;
	proxy.type = Type::Socks5;
	auto port = QString();
	if (line.contains(u"://"_q)) {
		const auto url = QUrl(line, QUrl::StrictMode);
		if (!url.isValid()) {
			return Error::Format;
		}
		const auto scheme = url.scheme().toLower();
		const auto telegram = (scheme == u"tg"_q)
			|| ((scheme == u"https"_q || scheme == u"http"_q)
				&& (url.host() == u"t.me"_q || url.host() == u"telegram.me"_q));
		if (telegram) {
			const auto kind = (scheme == u"tg"_q)
				? url.host() : url.path().mid(1);
			if (kind != u"socks"_q && kind != u"proxy"_q) {
				return Error::Protocol;
			}
			const auto query = QUrlQuery(url);
			proxy.type = (kind == u"proxy"_q) ? Type::Mtproto : Type::Socks5;
			proxy.host = query.queryItemValue(u"server"_q, QUrl::FullyDecoded);
			port = query.queryItemValue(u"port"_q);
			proxy.user = query.queryItemValue(u"user"_q, QUrl::FullyDecoded);
			proxy.password = query.queryItemValue(
				(proxy.type == Type::Mtproto) ? u"secret"_q : u"pass"_q,
				QUrl::FullyDecoded);
		} else if (scheme == u"socks5"_q || scheme == u"mtproto"_q) {
			proxy.type = (scheme == u"mtproto"_q) ? Type::Mtproto : Type::Socks5;
			proxy.host = url.host();
			port = QString::number(url.port(-1));
			proxy.user = url.userName(QUrl::FullyDecoded);
			proxy.password = url.password(QUrl::FullyDecoded);
			if (proxy.type == Type::Mtproto) {
				proxy.password = proxy.user;
				proxy.user.clear();
			}
			if (!url.path().isEmpty() || url.hasQuery() || url.hasFragment()) {
				return Error::Format;
			}
		} else {
			return Error::Protocol;
		}
	} else {
		auto endpoint = line;
		const auto at = endpoint.lastIndexOf('@');
		if (at >= 0) {
			const auto auth = endpoint.left(at);
			const auto colon = auth.indexOf(':');
			if (colon < 0) {
				return Error::Format;
			}
			proxy.user = auth.left(colon);
			proxy.password = auth.mid(colon + 1);
			endpoint = endpoint.mid(at + 1);
		}
		static const auto expression = QRegularExpression(
			u"^(\\[[^\\]]+\\]|[^:]+):([0-9]+)(?::([^:]*):(.*))?$"_q);
		const auto match = expression.match(endpoint);
		if (!match.hasMatch() || (at >= 0 && match.hasCaptured(3))) {
			return Error::Format;
		}
		proxy.host = match.captured(1);
		port = match.captured(2);
		if (match.hasCaptured(3)) {
			proxy.user = match.captured(3);
			proxy.password = match.captured(4);
		}
	}
	if (proxy.host.startsWith('[') && proxy.host.endsWith(']')) {
		proxy.host = proxy.host.mid(1, proxy.host.size() - 2);
	}
	auto address = QHostAddress();
	if (!address.setAddress(proxy.host) || !address.scopeId().isEmpty()) {
		return Error::Address;
	}
	proxy.host = address.toString();
	auto validPort = false;
	const auto numericPort = port.toUInt(&validPort);
	if (!validPort || !numericPort || numericPort > 65535) {
		return Error::Port;
	}
	proxy.port = numericPort;
	if (proxy.type == Type::Mtproto && !proxy.valid()) {
		return Error::Secret;
	}
	return Core::ProxyLibrary::Supported(proxy) ? Error::None : Error::Format;
}

} // namespace

std::vector<ProxyImportRow> ParseProxyImport(
		const QString &text,
		const std::vector<ProxyEntry> &existing,
		bool tryIPv6) {
	auto result = std::vector<ProxyImportRow>();
	if (text.size() > 1024 * 1024) {
		return { { .line = 1, .error = ProxyImportError::Limit } };
	}
	auto identities = QSet<QByteArray>();
	for (const auto &entry : existing) {
		identities.insert(Identity(entry.proxy));
	}
	const auto lines = text.split('\n');
	for (auto i = 0; i != lines.size(); ++i) {
		const auto line = lines[i].trimmed();
		if (line.isEmpty() || line.startsWith('#')) {
			continue;
		}
		auto row = ProxyImportRow{ .line = i + 1 };
		if (result.size() >= 10000) {
			row.error = ProxyImportError::Limit;
			result.push_back(std::move(row));
			break;
		}
		row.entry.tryIPv6 = tryIPv6;
		row.error = (line.size() > 8192)
			? ProxyImportError::Limit : ParseLine(line, row.entry.proxy);
		if (row.error == ProxyImportError::None) {
			const auto identity = Identity(row.entry.proxy);
			if (identities.contains(identity)) {
				row.error = ProxyImportError::Duplicate;
			} else {
				identities.insert(identity);
			}
		}
		if (row.error != ProxyImportError::None) {
			row.entry = {};
		}
		result.push_back(std::move(row));
	}
	return result;
}

} // namespace Core
