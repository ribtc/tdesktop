#include "main/main_protection.h"

#include "base/unixtime.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Main {
namespace {

constexpr auto kProtectionPref = "account-protection";
constexpr auto kEventLimit = 100;

} // namespace

Protection::Protection(not_null<Session*> session) : _session(session) {
	const auto bytes = session->local().readPref<QByteArray>(kProtectionPref);
	if (bytes.isEmpty()) {
		return;
	}
	const auto document = QJsonDocument::fromJson(bytes);
	const auto root = document.object();
	if (!document.isObject() || root["version"].toInt() != 1
		|| !root["loginSince"].isDouble() || !root["days"].isDouble()
		|| !root["retain"].isBool() || !root["preview"].isBool()
		|| !root["logins"].isBool() || !root["terminate"].isBool()
		|| !root["trusted"].isArray() || !root["pending"].isObject()
		|| !root["events"].isArray()
		|| (root["logins"].toBool() && root["loginSince"].toInt() <= 0)) {
		_writable = false;
		return;
	}

	for (const auto &value : root["trusted"].toArray()) {
		const auto hash = value.toString().toULongLong();
		if (!hash) {
			_writable = false;
			return;
		}
		_trusted.emplace(hash);
	}
	const auto pending = root["pending"].toObject();
	for (auto i = pending.begin(); i != pending.end(); ++i) {
		const auto hash = i.key().toULongLong();
		if (!hash || !i.value().isString()) {
			_writable = false;
			return;
		}
		_pending.emplace(hash, i.value().toString());
	}
	for (const auto &value : root["events"].toArray()) {
		const auto event = value.toObject();
		if (!event["date"].isDouble() || !event["text"].isString()) {
			_writable = false;
			return;
		}
		_events.push_back({ event["date"].toInt(), event["text"].toString() });
	}
	_settings = {
		.loginSince = root["loginSince"].toInt(),
		.retentionDays = std::clamp(root["days"].toInt(30), 1, 365),
		.retainDeleted = root["retain"].toBool(),
		.unreadPreview = root["preview"].toBool(),
		.guardLogins = root["logins"].toBool(),
		.terminateUnknown = root["terminate"].toBool(),
	};
}

bool Protection::writable() const {
	return _writable;
}

const ProtectionSettings &Protection::settings() const {
	return _settings;
}

void Protection::setSettings(ProtectionSettings settings) {
	if (!_writable) {
		return;
	}
	settings.retentionDays = std::clamp(settings.retentionDays, 1, 365);
	settings.loginSince = settings.guardLogins
		? (_settings.guardLogins ? _settings.loginSince : base::unixtime::now())
		: 0;
	if (settings == _settings) {
		return;
	}
	_settings = settings;
	record(tr::lng_protection_settings_updated(tr::now));
	_settingsChanges.fire({});
}

bool Protection::trusted(uint64 hash) const {
	return !hash || _trusted.contains(hash);
}

void Protection::trust(uint64 hash, bool trusted) {
	if (!hash || !_writable) {
		return;
	}
	if (trusted) {
		_trusted.emplace(hash);
		_pending.remove(hash);
	} else {
		_trusted.remove(hash);
	}
	save();
	_settingsChanges.fire({});
}

const base::flat_set<uint64> &Protection::trustedSessions() const {
	return _trusted;
}

const base::flat_map<uint64, QString> &Protection::pending() const {
	return _pending;
}

void Protection::setPending(uint64 hash, QString reason) {
	if (hash && _pending[hash] != reason) {
		_pending[hash] = std::move(reason);
		save();
	}
}

void Protection::resolve(uint64 hash) {
	if (_pending.remove(hash)) {
		save();
	}
}

void Protection::record(QString text) {
	_events.insert(_events.begin(), { base::unixtime::now(), std::move(text) });
	if (_events.size() > kEventLimit) {
		_events.resize(kEventLimit);
	}
	save();
}

const std::vector<ProtectionEvent> &Protection::events() const {
	return _events;
}

rpl::producer<> Protection::changes() const {
	return _changes.events();
}

rpl::producer<> Protection::settingsChanges() const {
	return _settingsChanges.events();
}

void Protection::save() {
	if (!_writable) {
		return;
	}
	auto trusted = QJsonArray();
	for (const auto hash : _trusted) {
		trusted.push_back(QString::number(hash));
	}
	auto pending = QJsonObject();
	for (const auto &[hash, reason] : _pending) {
		pending.insert(QString::number(hash), reason);
	}
	auto events = QJsonArray();
	for (const auto &event : _events) {
		events.push_back(QJsonObject{
			{ "date", qint64(event.date) },
			{ "text", event.text },
		});
	}
	_session->local().writePref<QByteArray>(
		kProtectionPref,
		QJsonDocument(QJsonObject{
			{ "version", 1 },
			{ "loginSince", qint64(_settings.loginSince) },
			{ "days", _settings.retentionDays },
			{ "retain", _settings.retainDeleted },
			{ "preview", _settings.unreadPreview },
			{ "logins", _settings.guardLogins },
			{ "terminate", _settings.terminateUnknown },
			{ "trusted", trusted },
			{ "pending", pending },
			{ "events", events },
		}).toJson(QJsonDocument::Compact));
	_changes.fire({});
}

} // namespace Main
