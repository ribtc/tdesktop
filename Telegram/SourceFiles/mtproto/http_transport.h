/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/network_context.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

namespace MTP {

class HttpTransport final : public QNetworkAccessManager {
public:
	explicit HttpTransport(NetworkSnapshot route);
	explicit HttpTransport(NetworkContext &context);
	~HttpTransport();

	void revoke();

protected:
	QNetworkReply *createRequest(
		Operation operation,
		const QNetworkRequest &request,
		QIODevice *outgoingData) override;

private:
	const NetworkSnapshot _route;
	rpl::lifetime _lifetime;
	bool _revoked = false;

};

} // namespace MTP
