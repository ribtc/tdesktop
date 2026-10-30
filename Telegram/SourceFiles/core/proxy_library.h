/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/network_context.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace Core {

inline constexpr auto kProxyLibraryPref = "account-proxy-library";

struct ProxyEntry {
	QString id;
	MTP::ProxyData proxy;
	QString notes;
	uint64 revision = 1;
	bool tryIPv6 = false;
	QJsonObject preserved;
};

struct ProxyLibraryChange {
	QString id;
	bool before = false;
	bool connection = false;
};

class ProxyLibrary final {
public:
	[[nodiscard]] bool load(const QByteArray &bytes);
	[[nodiscard]] QByteArray serialize() const;
	[[nodiscard]] bool writable() const;
	[[nodiscard]] const std::vector<ProxyEntry> &entries() const;
	[[nodiscard]] const ProxyEntry *find(const QString &id) const;
	[[nodiscard]] static bool Supported(const MTP::ProxyData &proxy);
	[[nodiscard]] QString add(
		MTP::ProxyData proxy,
		QString notes = {},
		bool tryIPv6 = false);
	bool update(const ProxyEntry &entry);
	bool remove(const QString &id);
	[[nodiscard]] std::vector<QString> identifiers() const;
	bool clear();
	void importLegacy(
		const std::vector<MTP::ProxyData> &proxies,
		const MTP::ProxyData &selected,
		bool tryIPv6);
	[[nodiscard]] rpl::producer<ProxyLibraryChange> changes() const;

private:
	QString append(MTP::ProxyData proxy, QString notes, bool tryIPv6);
	std::vector<ProxyEntry> _entries;
	QJsonObject _preserved;
	QJsonArray _quarantined;
	QByteArray _unreadable;
	bool _writable = true;
	bool _migrated = false;
	rpl::event_stream<ProxyLibraryChange> _changes;

};

} // namespace Core
