/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/connection_abstract.h"

namespace MTP {

class ProxyCheckConnection final {
public:
	ProxyCheckConnection();
	ProxyCheckConnection(ProxyCheckConnection &&other);
	ProxyCheckConnection &operator=(ProxyCheckConnection &&other);
	ProxyCheckConnection &operator=(std::nullptr_t);
	~ProxyCheckConnection();

	[[nodiscard]] details::AbstractConnection *get() const;
	[[nodiscard]] explicit operator bool() const;

private:
	friend void StartProxyCheck(
		not_null<Instance*> mtproto,
		const ProxyData &proxy,
		bool tryIPv6,
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6,
		Fn<void(details::AbstractConnection *raw, int ping)> done,
		Fn<void(details::AbstractConnection *raw)> fail);

	class Private;
	std::unique_ptr<Private> _private;

};

void ResetProxyCheckers(
	ProxyCheckConnection &v4,
	ProxyCheckConnection &v6);
void DropProxyChecker(
	ProxyCheckConnection &v4,
	ProxyCheckConnection &v6,
	not_null<details::AbstractConnection*> raw);
[[nodiscard]] bool HasProxyCheckers(
	const ProxyCheckConnection &v4,
	const ProxyCheckConnection &v6);
void StartProxyCheck(
	not_null<Instance*> mtproto,
	const ProxyData &proxy,
	bool tryIPv6,
	ProxyCheckConnection &v4,
	ProxyCheckConnection &v6,
	Fn<void(details::AbstractConnection *raw, int ping)> done,
	Fn<void(details::AbstractConnection *raw)> fail);

} // namespace MTP
