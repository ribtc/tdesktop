#include "core/proxy_import.h"
#include "core/proxy_check_queue.h"

#include <QtCore/QCoreApplication>
#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}
} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	const auto rows = Core::ParseProxyImport(
		u"# comment\n\n192.0.2.1:1080:user:private\n"
		"user:private@[2001:db8::1]:1081\n"
		"socks5://user:private@192.0.2.1:1080\n"_q,
		{},
		true);
	Require(rows.size() == 3 && rows[0].line == 3,
		"Comments and empty lines preserve source line numbers.");
	Require(rows[0].error == Core::ProxyImportError::None
		&& rows[0].entry.proxy.user == u"user"_q
		&& rows[0].entry.proxy.password == u"private"_q
		&& rows[0].entry.tryIPv6,
		"SOCKS shorthand keeps credentials and the requested address family.");
	Require(rows[1].error == Core::ProxyImportError::None
		&& rows[1].entry.proxy.host == u"2001:db8::1"_q,
		"Bracketed IPv6 is accepted without a DNS lookup.");
	Require(rows[2].error == Core::ProxyImportError::Duplicate,
		"Equivalent explicit and implicit SOCKS proxies are deduplicated.");
	const auto links = Core::ParseProxyImport(
		u"tg://proxy?server=192.0.2.2&port=443&secret=11111111111111111111111111111111\n"
		"https://t.me/socks?server=192.0.2.3&port=1080&user=test&pass=p%40ss\n"
		"tg://proxy?server=192.0.2.4&port=443&secret=private\n"
		"socks5://test:private@proxy.example:1080\n"
		"192.0.2.5:65536:test:private\n"
		"https://untrusted.invalid/private\n"_q, {}, false);
	Require(links.size() == 6
		&& links[0].error == Core::ProxyImportError::None
		&& links[0].entry.proxy.type == MTP::ProxyData::Type::Mtproto
		&& links[1].entry.proxy.password == u"p@ss"_q,
		"Telegram proxy links retain protocol and decode credentials.");
	Require(links[2].error == Core::ProxyImportError::Secret
		&& links[3].error == Core::ProxyImportError::Address
		&& links[4].error == Core::ProxyImportError::Port
		&& links[5].error == Core::ProxyImportError::Protocol,
		"Malformed links report classified errors without following URLs.");
	for (auto i = 2; i != 6; ++i) {
		Require(links[i].entry.proxy.password.isEmpty()
			&& links[i].entry.proxy.host.isEmpty(),
			"Rejected rows cannot leak input through previews or errors.");
	}
	auto callbacks = std::vector<Fn<void(int)>>();
	auto running = 0;
	auto results = 0;
	auto checks = Core::ProxyCheckQueue([&](const Core::ProxyEntry &, Fn<void(int)> done) {
		++running;
		callbacks.push_back(std::move(done));
		return rpl::lifetime([&] { --running; });
	});
	auto life = rpl::lifetime();
	checks.results() | rpl::on_next([&](Core::ProxyCheckResult result) {
		if (result.state == Core::ProxyCheckState::Available) {
			++results;
		}
	}, life);
	auto entries = std::vector<Core::ProxyEntry>(9);
	checks.start(entries);
	Require(running == 4 && callbacks.size() == 4,
		"Batch checks have at most four concurrent protocol checks.");
	const auto first = callbacks[0];
	first(12);
	Require(running == 4 && callbacks.size() == 5 && results == 1,
		"Completing a check starts one queued proxy and publishes measured latency.");
	checks.cancel();
	Require(running == 0, "Cancel destroys all active check lifetimes.");
	for (const auto &done : callbacks) {
		done(1);
	}
	Require(results == 1 && callbacks.size() == 5,
		"Late callbacks after cancel cannot publish results or start queued checks.");
	auto startsAfterCancel = 0;
	auto reentrant = Core::ProxyCheckQueue([&](const Core::ProxyEntry &, Fn<void(int)>) {
		++startsAfterCancel;
		return rpl::lifetime();
	});
	reentrant.results() | rpl::on_next([&](Core::ProxyCheckResult result) {
		if (result.state == Core::ProxyCheckState::Checking) {
			reentrant.cancel();
		}
	}, life);
	reentrant.start(entries);
	Require(startsAfterCancel == 0,
		"Cancellation from a status observer prevents the pending connection start.");
	auto independent = Core::ProxyCheckQueue();
	auto independentCallbacks = std::vector<Fn<void(int)>>();
	auto owners = std::vector<rpl::lifetime>();
	auto independentRunning = 0;
	auto independentResults = 0;
	const auto runner = [&](const Core::ProxyEntry &, Fn<void(int)> done) {
		++independentRunning;
		independentCallbacks.push_back(std::move(done));
		return rpl::lifetime([&] { --independentRunning; });
	};
	for (auto i = 0; i != 500; ++i) {
		owners.push_back(independent.enqueue({}, runner, [&](int) {
			++independentResults;
		}));
	}
	Require(independentRunning == 4 && independentCallbacks.size() == 4,
		"Restoring 500 independent owners shares the four-check limit.");
	owners[4].destroy();
	owners[0].destroy();
	Require(independentRunning == 4 && independentCallbacks.size() == 5,
		"Cancelling one running owner starts the next uncancelled queued owner.");
	independentCallbacks[0](7);
	Require(independentResults == 0 && independentRunning == 4,
		"A cancelled owner's late success cannot publish or free another slot.");
	const auto second = independentCallbacks[1];
	second(-1);
	Require(independentResults == 1 && independentRunning == 4,
		"A failure completes one owner and admits another queued owner.");
	independent.cancel();
	Require(independentRunning == 0, "Cancelling the queue stops all owners.");
	for (const auto &done : independentCallbacks) {
		done(5);
	}
	Require(independentResults == 1,
		"No cancelled owner can deliver a late result after queue cancellation.");

	auto cancelOnStart = Core::ProxyCheckQueue();
	auto pendingOwner = rpl::lifetime();
	auto held = std::vector<rpl::lifetime>();
	for (auto i = 0; i != 4; ++i) {
		held.push_back(cancelOnStart.enqueue({}, runner, [](int) {}));
	}
	pendingOwner = cancelOnStart.enqueue({}, runner, [](int) {});
	auto observer = rpl::lifetime();
	cancelOnStart.results() | rpl::on_next([&](Core::ProxyCheckResult result) {
		if (result.state == Core::ProxyCheckState::Checking) {
			pendingOwner.destroy();
		}
	}, observer);
	const auto startedBeforeCancel = independentCallbacks.size();
	held[0].destroy();
	Require(independentCallbacks.size() == startedBeforeCancel,
		"Cancelling an owner from its Checking event must not start its connection.");
	auto cancelOnResult = Core::ProxyCheckQueue();
	auto completedAfterCancel = false;
	auto resultObserver = rpl::lifetime();
	cancelOnResult.results() | rpl::on_next([&](Core::ProxyCheckResult result) {
		if (result.state == Core::ProxyCheckState::Available) {
			cancelOnResult.cancel();
		}
	}, resultObserver);
	auto resultOwner = cancelOnResult.enqueue({}, runner, [&](int) {
		completedAfterCancel = true;
	});
	const auto completeResult = independentCallbacks.back();
	completeResult(12);
	Require(!completedAfterCancel,
		"Cancelling from a result observer suppresses the owner's completion callback.");
	auto existing = Core::ProxyEntry();
	existing.proxy.type = MTP::ProxyData::Type::Socks5;
	existing.proxy.host = u"2001:0db8:0000::1"_q;
	existing.proxy.port = 1080;
	const auto equivalent = Core::ParseProxyImport(u"[2001:db8::1]:1080"_q, {existing}, false);
	Require(equivalent.front().error == Core::ProxyImportError::Duplicate,
		"Equivalent IPv6 spellings cannot bypass existing-library deduplication.");
}
