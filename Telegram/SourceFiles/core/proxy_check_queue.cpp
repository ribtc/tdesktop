#include "core/proxy_check_queue.h"

#include <deque>

namespace Core {

struct ProxyCheckQueue::State {
	struct Job {
		ProxyEntry entry;
		rpl::lifetime lifetime;
	};
	Runner runner;
	std::deque<ProxyEntry> pending;
	std::vector<std::shared_ptr<Job>> active;
	rpl::event_stream<ProxyCheckResult> results;
	uint64 generation = 0;
	bool pumping = false;
};

ProxyCheckQueue::ProxyCheckQueue(Runner runner)
: _state(std::make_shared<State>()) {
	_state->runner = std::move(runner);
}

ProxyCheckQueue::~ProxyCheckQueue() {
	cancel();
}

void ProxyCheckQueue::start(std::vector<ProxyEntry> entries) {
	cancel();
	_state->pending.assign(entries.begin(), entries.end());
	Pump(_state);
}

void ProxyCheckQueue::cancel() {
	++_state->generation;
	_state->pending.clear();
	const auto active = std::exchange(_state->active, {});
	for (const auto &job : active) {
		job->lifetime.destroy();
		_state->results.fire({
			job->entry.id,
			job->entry.revision,
			ProxyCheckState::Cancelled,
		});
	}
}

void ProxyCheckQueue::Pump(const std::shared_ptr<State> &state) {
	if (state->pumping) {
		return;
	}
	state->pumping = true;
	while (!state->pending.empty() && state->active.size() < 4) {
		const auto generation = state->generation;
		const auto job = std::make_shared<State::Job>();
		job->entry = std::move(state->pending.front());
		state->pending.pop_front();
		state->active.push_back(job);
		state->results.fire({ job->entry.id, job->entry.revision });
		if (state->generation != generation) {
			continue;
		}
		const auto weak = std::weak_ptr<State>(state);
		const auto weakJob = std::weak_ptr<State::Job>(job);
		auto lifetime = state->runner(job->entry, [=](int ping) {
			const auto state = weak.lock();
			const auto job = weakJob.lock();
			if (!state || !job || state->generation != generation) {
				return;
			}
			const auto i = std::find(state->active.begin(), state->active.end(), job);
			if (i == state->active.end()) {
				return;
			}
			state->active.erase(i);
			job->lifetime.destroy();
			state->results.fire({
				job->entry.id,
				job->entry.revision,
				(ping >= 0) ? ProxyCheckState::Available : ProxyCheckState::Unavailable,
				std::max(ping, 0),
			});
			Pump(state);
		});
		if (state->generation == generation
			&& std::find(state->active.begin(), state->active.end(), job)
				!= state->active.end()) {
			job->lifetime = std::move(lifetime);
		}
	}
	state->pumping = false;
}

rpl::producer<ProxyCheckResult> ProxyCheckQueue::results() const {
	return _state->results.events();
}

} // namespace Core
