/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/network_context.h"

#include <QtNetwork/QHostAddress>

namespace MTP {

bool NetworkSnapshot::current() const {
	return _current && _current->load();
}

bool NetworkSnapshot::allowed() const {
	return current()
		&& (state == NetworkState::Legacy || state == NetworkState::Ready);
}

std::optional<QNetworkProxy> NetworkSnapshot::httpProxy() const {
	if (!allowed()
		|| proxy.type != ProxyData::Type::Socks5
		|| !proxy.valid()
		|| QHostAddress(proxy.host).isNull()) {
		return std::nullopt;
	}
	return QNetworkProxy(
		QNetworkProxy::Socks5Proxy,
		proxy.host,
		proxy.port,
		proxy.user,
		proxy.password);
}

NetworkContext::NetworkContext(Mode mode) {
	_snapshot.state = (mode == Mode::Legacy)
		? NetworkState::Legacy
		: NetworkState::Unbound;
	_snapshot._current = std::make_shared<std::atomic<bool>>(true);
}

NetworkContext::~NetworkContext() {
	_snapshot._current->store(false);
	_changes.fire({});
}

NetworkSnapshot NetworkContext::snapshot() const {
	const auto lock = std::lock_guard(_mutex);
	return _snapshot;
}

NetworkSnapshot NetworkContext::bind(
		QString bindingId,
		ProxyData proxy,
		bool tryIPv6) {
	auto next = NetworkSnapshot();
	next.bindingId = std::move(bindingId);
	next.proxy = std::move(proxy);
	next.tryIPv6 = tryIPv6;
	next.state = next.bindingId.isEmpty()
		? NetworkState::Unbound
		: NetworkState::Checking;
	replace(std::move(next));
	return snapshot();
}

void NetworkContext::block() {
	auto next = snapshot();
	next.state = next.bindingId.isEmpty()
		? NetworkState::Unbound
		: NetworkState::Failed;
	replace(std::move(next));
}

void NetworkContext::applyLegacy(ProxyData proxy, bool enabled, bool tryIPv6) {
	auto next = snapshot();
	if (next.state != NetworkState::Legacy) {
		return;
	}
	next.proxy = enabled ? std::move(proxy) : ProxyData();
	next.tryIPv6 = tryIPv6;
	replace(std::move(next));
}

bool NetworkContext::completeValidation(
		const NetworkSnapshot &check,
		bool success) {
	auto next = snapshot();
	if (next._current != check._current
		|| next.state != NetworkState::Checking
		|| (success && !next.proxy.valid())) {
		return false;
	}
	next.state = success ? NetworkState::Ready : NetworkState::Failed;
	replace(std::move(next));
	return true;
}

bool NetworkContext::updateResolved(
		const NetworkSnapshot &request,
		std::vector<QString> ips,
		crl::time expireAt) {
	Expects(std::this_thread::get_id() == _ownerThread);
	const auto lock = std::lock_guard(_mutex);
	if (_snapshot._current != request._current
		|| _snapshot.state != NetworkState::Legacy) {
		return false;
	}
	_snapshot.proxy.resolvedIPs = std::move(ips);
	_snapshot.proxy.resolvedExpireAt = expireAt;
	return true;
}

rpl::producer<> NetworkContext::changes() const {
	return _changes.events();
}

void NetworkContext::replace(NetworkSnapshot next) {
	Expects(std::this_thread::get_id() == _ownerThread);
	{
		const auto lock = std::lock_guard(_mutex);
		_snapshot._current->store(false);
		next._current = std::make_shared<std::atomic<bool>>(true);
		next.generation = _snapshot.generation + 1;
		_snapshot = std::move(next);
	}
	_changes.fire({});
}

} // namespace MTP
