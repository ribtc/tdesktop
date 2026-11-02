#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "main/session_file.h"
#include "mtproto/network_context.h"
#include "mtproto/proxy_check.h"
#include <QtCore/QObject>
#include <optional>

namespace MTP {
class AuthKey;
class Instance;
class Sender;
} // namespace MTP

namespace Main {

class Account;

enum class SessionImportStatus {
	Waiting,
	Reading,
	Connecting,
	Imported,
	Duplicate,
	Unreadable,
	Unsupported,
	Invalid,
	Unauthorized,
	NetworkError,
	ProxyUnsupported,
	Cancelled,
};

struct SessionImportUpdate {
	int index = 0;
	SessionImportStatus status = SessionImportStatus::Waiting;
};

class SessionImport final : public QObject {
public:
	explicit SessionImport(not_null<Account*> source);
	~SessionImport();
	void start(QStringList paths);
	void cancel();
	[[nodiscard]] rpl::producer<SessionImportUpdate> updates() const;
	[[nodiscard]] Account *firstImported() const;

private:
	void next();
	void connect(SessionFileResult result);
	void prepareRoute(const SessionCredentials &credentials);
	void startConnection();
	[[nodiscard]] rpl::lifetime runProxyCheck(Fn<void(int)> done);
	void finish(SessionImportStatus status);
	void accept(const MTPUser &user);
	void commitImport(const MTPUser &user);
	void closeConnection();

	base::weak_ptr<Account> _source;
	base::weak_ptr<Account> _firstImported;
	MTP::NetworkSnapshot _route;
	std::unique_ptr<MTP::NetworkContext> _routeContext;
	bool _inheritRoute = false;
	MTP::ProxyCheckConnection _proxyCheckV4;
	MTP::ProxyCheckConnection _proxyCheckV6;
	rpl::lifetime _proxyCheckLifetime;
	QStringList _paths;
	int _index = -1;
	bool _cancelled = false;
	bool _finishing = false;
	bool _completed = false;
	std::optional<uint64> _reservedKeyId;
	std::shared_ptr<MTP::NetworkContext> _network;
	std::shared_ptr<MTP::AuthKey> _key;
	std::unique_ptr<MTP::Instance> _mtp;
	std::unique_ptr<MTP::Sender> _sender;
	base::Timer _timeout;
	rpl::event_stream<SessionImportUpdate> _updates;
	rpl::lifetime _lifetime;

};

} // namespace Main
