#pragma once

#include "core/proxy_library.h"

namespace Core {

enum class ProxyCheckState {
	Checking,
	Available,
	Unavailable,
	Cancelled,
};

struct ProxyCheckResult {
	QString id;
	uint64 revision = 0;
	ProxyCheckState state = ProxyCheckState::Checking;
	int ping = 0;
};

class ProxyCheckQueue final {
public:
	using Runner = Fn<rpl::lifetime(const ProxyEntry &, Fn<void(int)>)>;
	explicit ProxyCheckQueue(Runner runner = {});
	~ProxyCheckQueue();
	ProxyCheckQueue(const ProxyCheckQueue &) = delete;
	ProxyCheckQueue &operator=(const ProxyCheckQueue &) = delete;
	void start(std::vector<ProxyEntry> entries);
	void cancel();
	[[nodiscard]] rpl::lifetime enqueue(
		ProxyEntry entry,
		Runner runner,
		Fn<void(int)> done);
	[[nodiscard]] rpl::producer<ProxyCheckResult> results() const;

private:
	struct State;
	static void Pump(const std::shared_ptr<State> &state);
	std::shared_ptr<State> _state;

};

} // namespace Core
