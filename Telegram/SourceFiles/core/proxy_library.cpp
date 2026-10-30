/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/proxy_library.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtCore/QHash>

#include <limits>
#include <QtNetwork/QHostAddress>

namespace Core {

bool ProxyLibrary::load(const QByteArray &bytes) {
	_entries.clear();
	_quarantined = {};
	_preserved = {};
	_migrated = false;
	_unreadable = bytes;
	_writable = false;
	if (bytes.size() > 8 * 1024 * 1024) {
		return false;
	}
	const auto document = QJsonDocument::fromJson(bytes);
	const auto root = document.object();
	const auto records = root.value("entries").toArray();
	if (!document.isObject()
		|| root.value("version").toInt() != 1
		|| !root.value("entries").isArray()
		|| records.size() > 10000) {
		return false;
	}
	auto identities = QHash<QUuid, int>();
	for (const auto &record : records) {
		++identities[QUuid(record.toObject().value("id").toString())];
	}
	for (const auto &record : records) {
		const auto data = record.toObject();
		auto entry = ProxyEntry();
		entry.id = data.value("id").toString();
		auto revisionOk = false;
		entry.revision = data.value("revision").toString()
			.toULongLong(&revisionOk);
		const auto type = data.value("type").toInt(-1);
		const auto port = data.value("port").toInt(-1);
		if (QUuid(entry.id).isNull()
			|| identities.value(QUuid(entry.id)) != 1
			|| type < int(MTP::ProxyData::Type::Socks5)
			|| type > int(MTP::ProxyData::Type::Web)
			|| port <= 0 || port > 65535
			|| !revisionOk || !entry.revision
			|| entry.revision == std::numeric_limits<uint64>::max()
			|| !data.value("type").isDouble()
			|| !data.value("host").isString()
			|| !data.value("port").isDouble()
			|| !data.value("user").isString()
			|| !data.value("password").isString()
			|| !data.value("notes").isString()
			|| !data.value("ipv6").isBool()
			|| !data.value("revision").isString()
			|| data.value("type").toInt() != data.value("type").toDouble()
			|| data.value("port").toInt() != data.value("port").toDouble()) {
			_quarantined.push_back(record);
			continue;
		}
		entry.proxy.type = MTP::ProxyData::Type(data.value("type").toInt());
		entry.proxy.host = data.value("host").toString();
		entry.proxy.port = data.value("port").toInt();
		entry.proxy.user = data.value("user").toString();
		entry.proxy.password = data.value("password").toString();
		entry.notes = data.value("notes").toString();
		entry.tryIPv6 = data.value("ipv6").toBool();
		entry.preserved = data;
		_entries.push_back(std::move(entry));
	}
	_preserved = root;
	_migrated = root.value("migrated").toBool();
	_unreadable.clear();
	_writable = true;
	return true;
}

QByteArray ProxyLibrary::serialize() const {
	if (!_writable) {
		return _unreadable;
	}
	auto records = _quarantined;
	for (const auto &entry : _entries) {
		auto data = entry.preserved;
		data.insert("id", entry.id);
		data.insert("type", int(entry.proxy.type));
		data.insert("host", entry.proxy.host);
		data.insert("port", int(entry.proxy.port));
		data.insert("user", entry.proxy.user);
		data.insert("password", entry.proxy.password);
		data.insert("notes", entry.notes);
		data.insert("ipv6", entry.tryIPv6);
		data.insert("revision", QString::number(entry.revision));
		records.push_back(data);
	}
	auto root = _preserved;
	root.insert("version", 1);
	root.insert("migrated", _migrated);
	root.insert("entries", records);
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool ProxyLibrary::writable() const {
	return _writable;
}

const std::vector<ProxyEntry> &ProxyLibrary::entries() const {
	return _entries;
}

const ProxyEntry *ProxyLibrary::find(const QString &id) const {
	for (const auto &entry : _entries) {
		if (entry.id == id) {
			return &entry;
		}
	}
	return nullptr;
}

bool ProxyLibrary::Supported(const MTP::ProxyData &proxy) {
	return (proxy.type == MTP::ProxyData::Type::Socks5
		|| proxy.type == MTP::ProxyData::Type::Mtproto)
		&& proxy.valid()
		&& proxy.port <= 65535
		&& QHostAddress().setAddress(proxy.host);
}

QString ProxyLibrary::add(
		MTP::ProxyData proxy,
		QString notes,
		bool tryIPv6) {
	if (!_writable || !Supported(proxy)) {
		return {};
	}
	return append(std::move(proxy), std::move(notes), tryIPv6);
}

QString ProxyLibrary::append(
		MTP::ProxyData proxy,
		QString notes,
		bool tryIPv6) {
	auto entry = ProxyEntry();
	entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	entry.proxy = std::move(proxy);
	entry.notes = std::move(notes);
	entry.tryIPv6 = tryIPv6;
	_entries.push_back(entry);
	_changes.fire({ entry.id, false, true });
	return entry.id;
}

bool ProxyLibrary::update(const ProxyEntry &entry) {
	const auto old = find(entry.id);
	if (!_writable || !old || old->revision != entry.revision) {
		return false;
	}
	const auto connection = old->proxy != entry.proxy
		|| old->tryIPv6 != entry.tryIPv6;
	if (connection && (!Supported(entry.proxy)
		|| old->revision == std::numeric_limits<uint64>::max() - 1)) {
		return false;
	}
	const auto revision = old->revision + (connection ? 1 : 0);
	const auto index = old - _entries.data();
	_changes.fire({ entry.id, true, connection });
	_entries[index] = entry;
	_entries[index].revision = revision;
	_changes.fire({ entry.id, false, connection });
	return true;
}

bool ProxyLibrary::remove(const QString &id) {
	const auto entry = find(id);
	if (!_writable || !entry) {
		return false;
	}
	const auto index = entry - _entries.data();
	_changes.fire({ id, true, true });
	_entries.erase(_entries.begin() + index);
	_changes.fire({ id, false, true });
	return true;
}

void ProxyLibrary::importLegacy(
		const std::vector<MTP::ProxyData> &proxies,
		const MTP::ProxyData &selected,
		bool tryIPv6) {
	if (!_writable || _migrated) {
		return;
	}
	_migrated = true;
	for (const auto &proxy : proxies) {
		append(proxy, {}, tryIPv6);
	}
	if (selected.type != MTP::ProxyData::Type::None
		&& std::find(proxies.begin(), proxies.end(), selected) == proxies.end()) {
		append(selected, {}, tryIPv6);
	}
}

rpl::producer<ProxyLibraryChange> ProxyLibrary::changes() const {
	return _changes.events();
}

} // namespace Core
