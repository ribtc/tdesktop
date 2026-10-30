/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

#pragma once

#include "mtproto/network_context.h"

#include <QtCore/QByteArray>
#include <QtCore/QObject>

#include <functional>

namespace Core {

class WebSocketClient final : public QObject {
public:
	explicit WebSocketClient(QObject *parent = nullptr);
	WebSocketClient(MTP::NetworkContext &network, QObject *parent = nullptr);
	~WebSocketClient();

	void connectTo(
		const QString &host,
		int port,
		const QString &requestTarget,
		const QString &subprotocol);
	void sendText(const QByteArray &message);
	void sendBinary(const QByteArray &message);
	void close();

	std::function<void()> onConnected;
	std::function<void(QByteArray)> onText;
	std::function<void(QByteArray)> onBinary;
	std::function<void()> onClosed;
	std::function<void()> onActivity;

private:
	class Worker;
	struct Private;
	enum class Event {
		Connected,
		Text,
		Binary,
		Closed,
		Activity,
	};

	void dispatch(Event event, QByteArray payload);
	void fail();

	MTP::NetworkSnapshot _route;
	std::unique_ptr<Private> _private;
	bool _started = false;
	bool _closed = false;
	rpl::lifetime _lifetime;

};

} // namespace Core
