#include "data/components/retained_messages.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_protection.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

#include <QtCore/QDataStream>

namespace Data {
namespace {

constexpr auto kRetainedPref = "retained-messages";
constexpr auto kDay = TimeId(86400);

bool CanRemember(const MTPDmessage &data) {
	if (data.is_out() || data.vttl_period() || data.vid().v <= 0
		|| data.is_from_scheduled()) {
		return false;
	}
	return !data.vmedia() || data.vmedia()->match([](const auto &media) {
		if constexpr (std::is_same_v<std::decay_t<decltype(media)>,
				MTPDmessageMediaPhoto>
			|| std::is_same_v<std::decay_t<decltype(media)>,
				MTPDmessageMediaDocument>) {
			return !media.vttl_seconds();
		} else {
			return true;
		}
	});
}

} // namespace

RetainedMessages::RetainedMessages(not_null<Main::Session*> session)
: _session(session)
, _saveTimer([=] { save(); })
, _pruneTimer([=] { prune(); }) {
	load();
	crl::on_main(session, [=] {
		prune();
		_session->protection().settingsChanges() | rpl::on_next([=] {
			prune();
		}, _lifetime);
	});
}

RetainedMessages::~RetainedMessages() {
	if (_saveTimer.isActive()) {
		save();
	}
}

bool RetainedMessages::available() const {
	return _writable;
}

void RetainedMessages::remember(const MTPMessage &message, bool allowNew) {
	const auto &settings = _session->protection().settings();
	if (!_writable || !settings.retainDeleted || message.type() != mtpc_message) {
		return;
	}
	const auto &data = message.c_message();
	if (!CanRemember(data)) {
		return;
	}
	const auto id = FullMsgId(peerFromMTP(data.vpeer_id()), data.vid().v);
	if (isRetained(id) || (!allowNew && !_entries.contains(id))) {
		return;
	}
	auto &entry = _entries[id];
	entry.message = message;
	if (!entry.received) {
		entry.received = base::unixtime::now();
	}
	scheduleSave();
}

bool RetainedMessages::retain(PeerId peer, MsgId message) {
	if (!_writable) {
		return false;
	}
	const auto i = peer
		? _entries.find(FullMsgId(peer, message))
		: ranges::find_if(_entries, [=](const auto &pair) {
			return pair.first.msg == message && !peerIsChannel(pair.first.peer);
		});
	if (i == _entries.end()) {
		return false;
	} else if (i->second.recalled) {
		return true;
	} else if (!_session->protection().settings().retainDeleted) {
		return false;
	}
	const auto item = _session->data().message(i->first);
	auto &entry = i->second;
	entry.recalled = base::unixtime::now();
	if (item) {
		_session->data().requestItemViewRefresh(item);
		_session->data().requestItemRepaint(item);
	}
	_session->protection().record(tr::lng_protection_retained_event(tr::now));
	if (const auto history = _session->data().historyLoaded(i->first.peer)) {
		_session->changes().historyUpdated(
			history,
			HistoryUpdate::Flag::ClientSideMessages);
	}
	scheduleSave();
	return true;
}

bool RetainedMessages::isRetained(FullMsgId id) const {
	const auto i = _entries.find(id);
	return i != _entries.end() && i->second.recalled;
}

void RetainedMessages::forget(FullMsgId id) {
	if (_entries.remove(id)) {
		scheduleSave();
	}
}

void RetainedMessages::forget(PeerId peer, TimeId first, TimeId last) {
	forgetMatching(peer, [=](const MTPDmessage &message) {
		return message.vdate().v > first && message.vdate().v < last;
	});
}

void RetainedMessages::forgetTopic(PeerId peer, MsgId root) {
	forgetMatching(peer, [=](const MTPDmessage &message) {
		const auto reply = message.vreply_to();
		const auto top = reply && reply->type() == mtpc_messageReplyHeader
			? reply->c_messageReplyHeader().vreply_to_top_id().value_or(
				reply->c_messageReplyHeader().vreply_to_msg_id().value_or(1))
			: 1;
		return message.vid().v == root || top == root;
	});
}

void RetainedMessages::forgetFrom(PeerId peer, PeerId sender) {
	forgetMatching(peer, [=](const MTPDmessage &message) {
		return message.vfrom_id() && peerFromMTP(*message.vfrom_id()) == sender;
	});
}

void RetainedMessages::forgetMatching(
		PeerId peer,
		Fn<bool(const MTPDmessage&)> match) {
	auto removed = std::vector<FullMsgId>();
	for (auto i = _entries.begin(); i != _entries.end();) {
		if (i->first.peer == peer && match(i->second.message.c_message())) {
			if (i->second.recalled) {
				removed.push_back(i->first);
			}
			i = _entries.erase(i);
		} else {
			++i;
		}
	}
	destroyRetained(removed);
	scheduleSave();
}

void RetainedMessages::destroyRetained(const std::vector<FullMsgId> &ids) {
	auto peers = base::flat_set<PeerId>();
	for (const auto id : ids) {
		peers.emplace(id.peer);
		if (const auto item = _session->data().message(id)) {
			_session->data().notifyItemsAboutToBeDestroyed({ item });
			item->destroy();
		}
	}
	for (const auto peer : peers) {
		if (const auto history = _session->data().historyLoaded(peer)) {
			_session->changes().historyUpdated(
				history,
				HistoryUpdate::Flag::ClientSideMessages);
		}
	}
}

std::vector<not_null<HistoryItem*>> RetainedMessages::items(
		not_null<History*> history,
		TimeId first,
		TimeId last) {
	auto result = std::vector<not_null<HistoryItem*>>();
	for (const auto &[id, entry] : _entries) {
		if (id.peer != history->peer->id || !entry.recalled) {
			continue;
		}
		const auto date = entry.message.c_message().vdate().v;
		if (date < first || date >= last) {
			continue;
		}
		const auto item = history->createItem(id.msg, entry.message, {}, false);
		result.push_back(item);
	}
	return result;
}

void RetainedMessages::prune() {
	const auto settings = _session->protection().settings();
	const auto cutoff = base::unixtime::now() - settings.retentionDays * kDay;
	auto expired = std::vector<FullMsgId>();
	for (auto i = _entries.begin(); i != _entries.end();) {
		const auto &entry = i->second;
		const auto date = entry.recalled ? entry.recalled : entry.received;
		if (date <= cutoff || (!entry.recalled && !settings.retainDeleted)) {
			if (entry.recalled) {
				expired.push_back(i->first);
			}
			i = _entries.erase(i);
		} else {
			++i;
		}
	}
	destroyRetained(expired);
	scheduleSave();
	_pruneTimer.callOnce(60 * 60 * 1000);
}

void RetainedMessages::scheduleSave() {
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(1000);
	}
}

void RetainedMessages::save() {
	if (!_writable) {
		return;
	}
	_saveTimer.cancel();
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << quint32(1) << quint32(_entries.size());
	for (const auto &[id, entry] : _entries) {
		auto buffer = mtpBuffer();
		entry.message.write(buffer);
		stream << qint64(entry.received) << qint64(entry.recalled)
			<< QByteArray(
				reinterpret_cast<const char*>(buffer.constData()),
				buffer.size() * sizeof(mtpPrime));
	}
	_session->local().writePref<QByteArray>(kRetainedPref, bytes);
}

void RetainedMessages::load() {
	const auto bytes = _session->local().readPref<QByteArray>(kRetainedPref);
	if (bytes.isEmpty()) {
		return;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = quint32();
	auto count = quint32();
	stream >> version >> count;
	if (version != 1 || count > quint32(bytes.size() / 20)) {
		_writable = false;
		return;
	}
	for (auto index = quint32(); index != count; ++index) {
		auto received = qint64();
		auto recalled = qint64();
		auto raw = QByteArray();
		stream >> received >> recalled >> raw;
		auto buffer = mtpBuffer(raw.size() / sizeof(mtpPrime));
		memcpy(buffer.data(), raw.constData(), buffer.size() * sizeof(mtpPrime));
		auto from = buffer.constData();
		const auto end = from + buffer.size();
		auto message = MTPMessage();
		if (stream.status() != QDataStream::Ok || raw.size() % sizeof(mtpPrime)
			|| !message.read(from, end) || from != end
			|| message.type() != mtpc_message) {
			_writable = false;
			_entries.clear();
			return;
		}
		const auto &data = message.c_message();
		_entries.emplace(
			FullMsgId(peerFromMTP(data.vpeer_id()), data.vid().v),
			Entry{ message, TimeId(received), TimeId(recalled) });
	}
}

} // namespace Data
