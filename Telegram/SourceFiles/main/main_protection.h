#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"
#include <rpl/event_stream.h>

namespace Main {

class Session;

struct ProtectionSettings {
	TimeId loginSince = 0;
	int retentionDays = 30;
	bool retainDeleted = false;
	bool unreadPreview = false;
	bool guardLogins = false;
	bool terminateUnknown = false;
	friend bool operator==(
		const ProtectionSettings &,
		const ProtectionSettings &) = default;
};

struct ProtectionEvent {
	TimeId date = 0;
	QString text;
};

class Protection final {
public:
	explicit Protection(not_null<Session*> session);
	[[nodiscard]] const ProtectionSettings &settings() const;
	[[nodiscard]] bool writable() const;
	void setSettings(ProtectionSettings settings);
	[[nodiscard]] bool trusted(uint64 hash) const;
	void trust(uint64 hash, bool trusted);
	[[nodiscard]] const base::flat_set<uint64> &trustedSessions() const;
	[[nodiscard]] const base::flat_map<uint64, QString> &pending() const;
	void setPending(uint64 hash, QString reason);
	void resolve(uint64 hash);
	void record(QString text);
	[[nodiscard]] const std::vector<ProtectionEvent> &events() const;
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] rpl::producer<> settingsChanges() const;

private:
	void save();

	const not_null<Session*> _session;
	ProtectionSettings _settings;
	base::flat_set<uint64> _trusted;
	base::flat_map<uint64, QString> _pending;
	std::vector<ProtectionEvent> _events;
	rpl::event_stream<> _changes;
	rpl::event_stream<> _settingsChanges;
	bool _writable = true;

};

} // namespace Main
