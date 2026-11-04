#pragma once

#include "base/timer.h"

class History;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Data {

class RetainedMessages final {
public:
	explicit RetainedMessages(not_null<Main::Session*> session);
	~RetainedMessages();
	[[nodiscard]] bool available() const;
	void remember(const MTPMessage &message, bool allowNew = true);
	bool retain(PeerId peer, MsgId message);
	[[nodiscard]] bool isRetained(FullMsgId id) const;
	void forget(FullMsgId id);
	void forget(
		PeerId peer,
		TimeId first = 0,
		TimeId last = std::numeric_limits<TimeId>::max());
	[[nodiscard]] std::vector<not_null<HistoryItem*>> items(
		not_null<History*> history,
		TimeId first = 0,
		TimeId last = std::numeric_limits<TimeId>::max());
	void forgetTopic(PeerId peer, MsgId root);
	void forgetFrom(PeerId peer, PeerId sender);

private:
	void destroyRetained(const std::vector<FullMsgId> &ids);
	void forgetMatching(PeerId peer, Fn<bool(const MTPDmessage&)> match);
	struct Entry {
		MTPMessage message;
		TimeId received = 0;
		TimeId recalled = 0;
	};
	void load();
	void save();
	void prune();
	void scheduleSave();

	const not_null<Main::Session*> _session;
	base::flat_map<FullMsgId, Entry> _entries;
	base::Timer _saveTimer;
	base::Timer _pruneTimer;
	bool _writable = true;
	rpl::lifetime _lifetime;

};

} // namespace Data
