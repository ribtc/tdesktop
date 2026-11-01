#include "main/session_import.h"

#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/sender.h"
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>
#include <set>

namespace Main {
namespace {

constexpr auto kLoginTimeout = crl::time(45000);

std::set<uint64> &ImportingKeys() {
	static auto result = std::set<uint64>();
	return result;
}

SessionImportStatus FileStatus(SessionFileError error) {
	switch (error) {
	case SessionFileError::Unreadable: return SessionImportStatus::Unreadable;
	case SessionFileError::Unsupported: return SessionImportStatus::Unsupported;
	case SessionFileError::Invalid: return SessionImportStatus::Invalid;
	case SessionFileError::Unauthorized: return SessionImportStatus::Unauthorized;
	}
	Unexpected("Session file error");
}

} // namespace

SessionImport::SessionImport(not_null<Account*> source)
: _source(source)
, _route(source->network().snapshot())
, _timeout([=] { finish(SessionImportStatus::NetworkError); }) {
	source->network().changes() | rpl::on_next([=] {
		if (!_route.current()) {
			cancel();
		}
	}, _lifetime);
}

SessionImport::~SessionImport() {
	closeConnection();
}

void SessionImport::start(QStringList paths) {
	Expects(_index == -1 && _paths.empty());

	_paths = std::move(paths);
	next();
}

void SessionImport::cancel() {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	closeConnection();
	for (auto i = std::max(_index + int(_completed), 0); i < _paths.size(); ++i) {
		_updates.fire({ i, SessionImportStatus::Cancelled });
	}
}

rpl::producer<SessionImportUpdate> SessionImport::updates() const {
	return _updates.events();
}

Account *SessionImport::firstImported() const {
	return _firstImported.get();
}

void SessionImport::next() {
	if (_cancelled || ++_index >= _paths.size()) {
		return;
	}
	_finishing = false;
	_completed = false;
	_updates.fire({ _index, SessionImportStatus::Reading });
	const auto path = _paths[_index];
	const auto deliver = crl::guard(this, [=](SessionFileResult result) {
		connect(std::move(result));
	});
	crl::async([=] {
		auto result = ReadSessionFile(path);
		crl::on_main([=, result = std::move(result)]() mutable {
			deliver(std::move(result));
		});
	});
}

void SessionImport::connect(SessionFileResult result) {
	if (_cancelled) {
		return;
	} else if (const auto error = std::get_if<SessionFileError>(&result)) {
		finish(FileStatus(*error));
		return;
	} else if (!_source || !_route.allowed()) {
		finish(SessionImportStatus::NetworkError);
		return;
	}
	const auto &credentials = std::get<SessionCredentials>(result);
	auto data = MTP::AuthKey::Data();
	std::transform(
		credentials.authKey.begin(),
		credentials.authKey.end(),
		data.begin(),
		[](unsigned char byte) { return gsl::byte(byte); });
	_key = std::make_shared<MTP::AuthKey>(
		MTP::AuthKey::Type::ReadFromFile,
		credentials.dcId,
		data);
	for (const auto &entry : _source->domain().accounts()) {
		for (const auto &key : entry.account->mtp().getKeysForWrite()) {
			if (key->equals(_key)) {
				finish(SessionImportStatus::Duplicate);
				return;
			}
		}
	}
	if (_route.state == MTP::NetworkState::Legacy) {
		finish(SessionImportStatus::NetworkError);
		return;
	}
	if (!ImportingKeys().insert(_key->keyId()).second) {
		finish(SessionImportStatus::Duplicate);
		return;
	}
	_reservedKeyId = _key->keyId();
	_network = std::make_shared<MTP::NetworkContext>(_route);
	auto fields = MTP::Instance::Fields();
	fields.config = std::make_unique<MTP::Config>(
		Core::App().fallbackProductionConfig());
	fields.network = _network;
	fields.mainDcId = credentials.dcId;
	fields.keys = { _key };
	fields.deviceModel = Platform::DeviceModelPretty();
	fields.systemVersion = Platform::SystemVersionPretty();
	_mtp = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::Normal,
		std::move(fields));
	_sender = std::make_unique<MTP::Sender>(_mtp.get());
	_updates.fire({ _index, SessionImportStatus::Connecting });
	_timeout.callOnce(kLoginTimeout);
	_sender->request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, MTP_inputUserSelf())
	)).done([=](const MTPVector<MTPUser> &users) {
		if (users.v.size() != 1
			|| users.v.front().type() != mtpc_user
			|| !users.v.front().c_user().is_self()
			|| !users.v.front().c_user().vid().v
			|| users.v.front().c_user().is_bot()) {
			finish(SessionImportStatus::Unauthorized);
			return;
		}
		accept(users.v.front());
	}).fail([=](const MTP::Error &error) {
		finish(error.code() == 401 || error.type() == u"AUTH_KEY_DUPLICATED"_q
			? SessionImportStatus::Unauthorized
			: SessionImportStatus::NetworkError);
	}).handleAllErrors().send();
}

void SessionImport::accept(const MTPUser &user) {
	if (_finishing || _cancelled) {
		return;
	}
	_finishing = true;
	_timeout.cancel();
	_network->block();
	_mtp->quiesceNetwork(crl::guard(this, [=] {
		crl::on_main(this, [=] { commitImport(user); });
	}));
}

void SessionImport::commitImport(const MTPUser &user) {
	if (_cancelled || !_source || !_route.current()) {
		return;
	}
	auto &domain = _source->domain();
	const auto key = _key;
	closeConnection();
	for (const auto &entry : domain.accounts()) {
		const auto session = entry.account->maybeSession();
		if (session
			&& !entry.account->mtp().isTestMode()
			&& session->userId() == UserId(user.c_user().vid())) {
			_finishing = false;
			finish(SessionImportStatus::Duplicate);
			return;
		}
	}
	const auto account = domain.addAuthorized(key, user, _route.bindingId);
	if (!_firstImported) {
		_firstImported = account;
	}
	_finishing = false;
	finish(SessionImportStatus::Imported);
}

void SessionImport::finish(SessionImportStatus status) {
	if (_cancelled || _finishing) {
		return;
	}
	_finishing = true;
	_timeout.cancel();
	crl::on_main(this, [=] {
		if (_cancelled) {
			return;
		}
		closeConnection();
		_completed = true;
		_updates.fire({ _index, status });
		crl::on_main(this, [=] { next(); });
	});
}

void SessionImport::closeConnection() {
	_timeout.cancel();
	if (_network) {
		_network->block();
	}
	_sender = nullptr;
	_mtp = nullptr;
	if (_reservedKeyId) {
		ImportingKeys().erase(*_reservedKeyId);
		_reservedKeyId.reset();
	}
	_key = nullptr;
	_network = nullptr;
}

} // namespace Main
