/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/bytes.h"
#include <crl/crl_time.h>
#include "mtproto/mtproto_proxy_data.h"
#include <rpl/event_stream.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace MTP {

enum class NetworkState {
	Unbound,
	Checking,
	Ready,
	Failed,
	Legacy,
};

struct NetworkSnapshot {
	QString bindingId;
	ProxyData proxy;
	uint64 generation = 0;
	NetworkState state = NetworkState::Unbound;
	bool tryIPv6 = false;

	[[nodiscard]] bool current() const;
	[[nodiscard]] bool allowed() const;

private:
	friend class NetworkContext;
	std::shared_ptr<std::atomic<bool>> _current;
};

class NetworkContext final {
public:
	enum class Mode {
		Isolated,
		Legacy,
	};

	explicit NetworkContext(Mode mode = Mode::Isolated);
	~NetworkContext();

	[[nodiscard]] NetworkSnapshot snapshot() const;
	// Mutations and subscriptions stay on the constructing thread.
	[[nodiscard]] NetworkSnapshot bind(
		QString bindingId,
		ProxyData proxy,
		bool tryIPv6);
	void block();
	void applyLegacy(ProxyData proxy, bool enabled, bool tryIPv6);
	bool updateResolved(
		const NetworkSnapshot &request,
		std::vector<QString> ips,
		crl::time expireAt);
	bool completeValidation(const NetworkSnapshot &check, bool success);
	[[nodiscard]] rpl::producer<> changes() const;

private:
	void replace(NetworkSnapshot snapshot);

	const std::thread::id _ownerThread = std::this_thread::get_id();
	mutable std::mutex _mutex;
	NetworkSnapshot _snapshot;
	rpl::event_stream<> _changes;

};

} // namespace MTP
