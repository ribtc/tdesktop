#include "core/proxy_check_queue.h"

#include "base/algorithm.h"

#include <deque>

namespace Core {

struct ProxyCheckQueue::State {
	struct Job {
		ProxyEntry entry;
		Runner runner;
		Fn<void(int)> done;
		rpl::lifetime lifetime;
	};
	Runner runner;
	std::deque<std::shared_ptr<Job>> pending;
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
	for (auto &entry : entries) {
		const auto job = std::make_shared<State::Job>();
		job->entry = std::move(entry);
		job->runner = _state->runner;
		_state->pending.push_back(job);
	}
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

rpl::lifetime ProxyCheckQueue::enqueue(
		ProxyEntry entry,
		Runner runner,
		Fn<void(int)> done) {
	const auto job = std::make_shared<State::Job>();
	job->entry = std::move(entry);
	job->runner = std::move(runner);
	job->done = std::move(done);
	_state->pending.push_back(job);
	const auto weak = std::weak_ptr<State>(_state);
	const auto weakJob = std::weak_ptr<State::Job>(job);
	Pump(_state);
	return rpl::lifetime([=] {
		const auto state = weak.lock();
		const auto job = weakJob.lock();
		if (!state || !job) {
			return;
		}
		std::erase(state->pending, job);
		std::erase(state->active, job);
		job->done = nullptr;
		job->lifetime.destroy();
		Pump(state);
	});
}

void ProxyCheckQueue::Pump(const std::shared_ptr<State> &state) {
	if (state->pumping) {
		return;
	}
	state->pumping = true;
	while (!state->pending.empty() && state->active.size() < 4) {
		const auto generation = state->generation;
		const auto job = state->pending.front();
		state->pending.pop_front();
		state->active.push_back(job);
		state->results.fire({ job->entry.id, job->entry.revision });
		if (state->generation != generation
			|| std::find(state->active.begin(), state->active.end(), job)
				== state->active.end()) {
			continue;
		}
		const auto weak = std::weak_ptr<State>(state);
		const auto weakJob = std::weak_ptr<State::Job>(job);
		auto lifetime = job->runner(job->entry, [=](int ping) {
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
			if (state->generation != generation) {
				return;
			}
			if (const auto done = base::take(job->done)) {
				done(ping);
			}
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
