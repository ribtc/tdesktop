/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_account.h"

#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h" // Storage::StartResult.
#include "storage/serialize_common.h"
#include "storage/serialize_peer.h"
#include "storage/localstorage.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "window/window_controller.h"
#include "media/audio/media_audio.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/network_context.h"
#include "mainwidget.h"
#include "api/api_updates.h"
#include "ui/ui_utility.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/main_session_settings.h"

namespace Main {
namespace {

constexpr auto kWideIdsTag = ~uint64(0);

[[nodiscard]] QString ComposeDataString(const QString &dataName, int index) {
	auto result = dataName;
	result.replace('#', QString());
	if (index > 0) {
		result += '#' + QString::number(index + 1);
	}
	return result;
}

} // namespace

Account::Account(not_null<Domain*> domain, const QString &dataName, int index)
: _domain(domain)
, _local(std::make_unique<Storage::Account>(
	this,
	ComposeDataString(dataName, index)))
, _network(std::make_shared<MTP::NetworkContext>()) {
}

Account::~Account() {
	cancelProxyCheck();
	_network->block();
	if (const auto session = maybeSession()) {
		session->saveSettingsNowIfNeeded();
		_local->writeSearchSuggestionsIfNeeded();
	}
	destroySession(DestroyReason::Quitting);
}

Storage::Domain &Account::domainLocal() const {
	return _domain->local();
}

[[nodiscard]] Storage::StartResult Account::legacyStart(
		const QByteArray &passcode) {
	Expects(!_appConfig);

	return _local->legacyStart(passcode);
}

std::unique_ptr<MTP::Config> Account::prepareToStart(
		std::shared_ptr<MTP::AuthKey> localKey) {
	return _local->start(std::move(localKey));
}

MTP::NetworkContext &Account::network() const {
	return *_network;
}

void Account::start(std::unique_ptr<MTP::Config> config) {
	watchProxyChanges();
	_appConfig = std::make_unique<AppConfig>(this);
	startMtp(config
		? std::move(config)
		: std::make_unique<MTP::Config>(
			Core::App().fallbackProductionConfig()));
	retryProxy();
	_appConfig->start();
	watchSessionChanges();
}

void Account::prepareToStartAdded(
		std::shared_ptr<MTP::AuthKey> localKey) {
	_local->startAdded(std::move(localKey));
}

QString Account::proxyBindingId() const {
	return _local->readPref<QString>("account-proxy-binding");
}

uint64 Account::proxyBindingOperation() const {
	return _proxyBindingOperation;
}

bool Account::pendingLogin() const {
	return _local->readPref<bool>("account-pending-login");
}

bool Account::bindProxy(const QString &id) {
	const auto entry = Core::App().settings().proxyLibrary().find(id);
	if (!entry || !Core::ProxyLibrary::Supported(entry->proxy)) {
		return false;
	}
	++_proxyBindingOperation;
	_proxyRetryDelay = 5000;
	_local->writePref<QString>("account-proxy-binding", id);
	_local->writePref<bool>("account-pending-login", !sessionExists());
	_domain->scheduleWriteAccounts();
	retryProxy();
	return true;
}

void Account::unbindProxy() {
	++_proxyBindingOperation;
	_proxyRetryDelay = 5000;
	cancelProxyCheck();
	_local->writePref<QString>("account-proxy-binding", QString());
	const auto unbound = _network->bind({}, {}, false);
	Assert(!unbound.allowed());
}

void Account::cancelPendingLogin() {
	if (!sessionExists()) {
		_local->writePref<bool>("account-pending-login", false);
		unbindProxy();
		_domain->scheduleWriteAccounts();
	}
}

void Account::cancelProxyCheck() {
	_proxyCheckLifetime.destroy();
	_proxyCheckTimer.cancel();
	MTP::ResetProxyCheckers(_proxyCheckV4, _proxyCheckV6);
}

void Account::retryProxy() {
	cancelProxyCheck();
	_network->block();
	if (!_mtp) {
		return;
	}
	const auto blocked = _network->snapshot();
	const auto remaining = std::make_shared<int>(_mtpForKeysDestroy ? 2 : 1);
	const auto closed = crl::guard(this, [=] {
		if (!--*remaining && blocked.current()) {
			startProxyCheck();
		}
	});
	_mtp->quiesceNetwork(closed);
	if (_mtpForKeysDestroy) {
		_mtpForKeysDestroy->quiesceNetwork(closed);
	}
}

void Account::startProxyCheck() {
	const auto id = proxyBindingId();
	const auto entry = Core::App().settings().proxyLibrary().find(id);
	const auto checking = _network->bind(
		id,
		entry ? entry->proxy : MTP::ProxyData(),
		entry && entry->tryIPv6);
	if (!entry || !Core::ProxyLibrary::Supported(entry->proxy)) {
		_network->completeValidation(checking, false);
		return;
	}
	_proxyCheckLifetime = _domain->proxyChecks().enqueue(
		*entry,
		crl::guard(this, [=](const Core::ProxyEntry &, Fn<void(int)> done) {
			return runProxyCheck(checking, std::move(done));
		}),
		crl::guard(this, [=](int ping) {
			if (_network->completeValidation(checking, ping >= 0)) {
				if (ping >= 0) {
					_proxyRetryDelay = 5000;
				} else {
					scheduleProxyRetry();
				}
			}
		}));
}

rpl::lifetime Account::runProxyCheck(
		MTP::NetworkSnapshot checking,
		Fn<void(int)> done) {
	_proxyCheckTimer.setCallback([=] { done(-1); });
	_proxyCheckTimer.callOnce(15000);
	MTP::StartProxyCheck(
		_mtp.get(),
		checking.proxy,
		checking.tryIPv6,
		_proxyCheckV4,
		_proxyCheckV6,
		crl::guard(this, [=](MTP::details::AbstractConnection*, int ping) {
			crl::on_main(this, [=] { done(ping); });
		}),
		crl::guard(this, [=](MTP::details::AbstractConnection *raw) {
			crl::on_main(this, [=] {
				if (!checking.current()) {
					return;
				}
				MTP::DropProxyChecker(_proxyCheckV4, _proxyCheckV6, raw);
				if (!MTP::HasProxyCheckers(_proxyCheckV4, _proxyCheckV6)) {
					done(-1);
				}
			});
		}));
	return rpl::lifetime([=] {
		_proxyCheckTimer.cancel();
		MTP::ResetProxyCheckers(_proxyCheckV4, _proxyCheckV6);
	});
}

void Account::scheduleProxyRetry() {
	const auto failed = _network->snapshot();
	if (failed.state != MTP::NetworkState::Failed) {
		return;
	}
	const auto operation = _proxyBindingOperation;
	_proxyCheckTimer.setCallback(crl::guard(this, [=] {
		if (failed.current() && operation == _proxyBindingOperation) {
			retryProxy();
		}
	}));
	_proxyCheckTimer.callOnce(_proxyRetryDelay);
	_proxyRetryDelay = std::min(_proxyRetryDelay * 2, crl::time(60000));
}

void Account::watchProxyChanges() {
	Core::App().settings().proxyLibrary().changes(
	) | rpl::on_next([=](Core::ProxyLibraryChange change) {
		if (change.id != proxyBindingId() || !change.connection) {
			return;
		}
		if (change.before) {
			++_proxyBindingOperation;
			_proxyRetryDelay = 5000;
			cancelProxyCheck();
			_network->block();
		} else if (Core::App().settings().proxyLibrary().find(change.id)) {
			retryProxy();
		} else {
			unbindProxy();
		}
	}, _lifetime);
}

void Account::watchSessionChanges() {
	sessionChanges(
	) | rpl::on_next([=](Session *session) {
		if (!session && _mtp) {
			_mtp->setUserPhone(QString());
		}
	}, _lifetime);
}

uint64 Account::willHaveSessionUniqueId(MTP::Config *config) const {
	// See also Session::uniqueId.
	if (!_sessionUserId) {
		return 0;
	}
	return _sessionUserId.bare
		| (config && config->isTestMode() ? 0x0100'0000'0000'0000ULL : 0ULL);
}

void Account::createSession(
		const MTPUser &user,
		std::unique_ptr<SessionSettings> settings) {
	createSession(
		user,
		QByteArray(),
		0,
		settings ? std::move(settings) : std::make_unique<SessionSettings>());
}

void Account::createSession(
		UserId id,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings) {
	DEBUG_LOG(("sessionUserSerialized.size: %1").arg(serialized.size()));
	QDataStream peekStream(serialized);
	const auto phone = Serialize::peekUserPhone(streamVersion, peekStream);
	const auto flags = MTPDuser::Flag::f_self | (phone.isEmpty()
		? MTPDuser::Flag()
		: MTPDuser::Flag::f_phone);

	createSession(
		MTP_user(
			MTP_flags(flags),
			MTP_long(base::take(_sessionUserId).bare),
			MTPlong(), // access_hash
			MTPstring(), // first_name
			MTPstring(), // last_name
			MTPstring(), // username
			MTP_string(phone),
			MTPUserProfilePhoto(),
			MTPUserStatus(),
			MTPint(), // bot_info_version
			MTPVector<MTPRestrictionReason>(),
			MTPstring(), // bot_inline_placeholder
			MTPstring(), // lang_code
			MTPEmojiStatus(),
			MTPVector<MTPUsername>(),
			MTPRecentStory(),
			MTPPeerColor(), // color
			MTPPeerColor(), // profile_color
			MTPint(), // bot_active_users
			MTPlong(), // bot_verification_icon
			MTPlong(), // send_paid_messages_stars
			MTPlong()), // linked_community_id
		serialized,
		streamVersion,
		std::move(settings));
}

void Account::createSession(
		const MTPUser &user,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings) {
	Expects(_mtp != nullptr);
	Expects(_session == nullptr);
	Expects(_sessionValue.current() == nullptr);

	_local->writePref<bool>("account-pending-login", false);
	_session = std::make_unique<Session>(this, user, std::move(settings));
	if (!serialized.isEmpty()) {
		local().readSelf(_session.get(), serialized, streamVersion);
	}
	_sessionValue = _session.get();

	Ensures(_session != nullptr);
}

void Account::destroySession(DestroyReason reason) {
	_storedSessionSettings.reset();
	_sessionUserId = 0;
	_sessionUserSerialized = {};
	if (!sessionExists()) {
		return;
	}

	// Assigning _sessionValue fires sessionChanges() synchronously, and a
	// listener may enter a nested event dispatch that drains crl::on_main.
	// Nothing may delete this Account while we're still on the stack.
	_destroyingSession = true;
	_sessionValue = nullptr;

	if (reason == DestroyReason::LoggedOut) {
		_session->finishLogout();
	}
	_session = nullptr;
	_destroyingSession = false;
}

bool Account::destroyingSession() const {
	return _destroyingSession;
}

bool Account::sessionExists() const {
	return (_sessionValue.current() != nullptr);
}

Session &Account::session() const {
	Expects(sessionExists());

	return *_sessionValue.current();
}

Session *Account::maybeSession() const {
	return _sessionValue.current();
}

rpl::producer<Session*> Account::sessionValue() const {
	return _sessionValue.value();
}

rpl::producer<Session*> Account::sessionChanges() const {
	return _sessionValue.changes();
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpValue() const {
	return _mtpValue.value() | rpl::map([](MTP::Instance *instance) {
		return not_null{ instance };
	});
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpMainSessionValue() const {
	return mtpValue() | rpl::map([=](not_null<MTP::Instance*> instance) {
		return instance->mainDcIdValue() | rpl::map_to(instance);
	}) | rpl::flatten_latest();
}

rpl::producer<MTPUpdates> Account::mtpUpdates() const {
	return _mtpUpdates.events();
}

rpl::producer<> Account::mtpNewSessionCreated() const {
	return _mtpNewSessionCreated.events();
}

void Account::setMtpMainDcId(MTP::DcId mainDcId) {
	Expects(!_mtp);

	_mtpFields.mainDcId = mainDcId;
}

void Account::setLegacyMtpKey(std::shared_ptr<MTP::AuthKey> key) {
	Expects(!_mtp);
	Expects(key != nullptr);

	_mtpFields.keys.push_back(std::move(key));
}

QByteArray Account::serializeMtpAuthorization() const {
	const auto serialize = [&](
			MTP::DcId mainDcId,
			const MTP::AuthKeysList &keys,
			const MTP::AuthKeysList &keysToDestroy) {
		const auto keysSize = [](auto &list) {
			const auto keyDataSize = MTP::AuthKey::Data().size();
			return sizeof(qint32)
				+ list.size() * (sizeof(qint32) + keyDataSize);
		};
		const auto writeKeys = [](
				QDataStream &stream,
				const MTP::AuthKeysList &keys) {
			stream << qint32(keys.size());
			for (const auto &key : keys) {
				stream << qint32(key->dcId());
				key->write(stream);
			}
		};

		auto result = QByteArray();
		// wide tag + userId + mainDcId
		auto size = 2 * sizeof(quint64) + sizeof(qint32);
		size += keysSize(keys) + keysSize(keysToDestroy);
		result.reserve(size);
		{
			QDataStream stream(&result, QIODevice::WriteOnly);
			stream.setVersion(QDataStream::Qt_5_1);

			const auto currentUserId = sessionExists()
				? session().userId()
				: UserId();
			stream
				<< quint64(kWideIdsTag)
				<< quint64(currentUserId.bare)
				<< qint32(mainDcId);
			writeKeys(stream, keys);
			writeKeys(stream, keysToDestroy);

			DEBUG_LOG(("MTP Info: Keys written, userId: %1, dcId: %2"
				).arg(currentUserId.bare
				).arg(mainDcId));
		}
		return result;
	};
	if (_mtp) {
		const auto keys = _mtp->getKeysForWrite();
		const auto keysToDestroy = _mtpForKeysDestroy
			? _mtpForKeysDestroy->getKeysForWrite()
			: MTP::AuthKeysList();
		return serialize(_mtp->mainDcId(), keys, keysToDestroy);
	}
	const auto &keys = _mtpFields.keys;
	const auto &keysToDestroy = _mtpKeysToDestroy;
	return serialize(_mtpFields.mainDcId, keys, keysToDestroy);
}

void Account::setSessionUserId(UserId userId) {
	Expects(!sessionExists());

	_sessionUserId = userId;
}

void Account::setSessionFromStorage(
		std::unique_ptr<SessionSettings> data,
		QByteArray &&selfSerialized,
		int32 selfStreamVersion) {
	Expects(!sessionExists());

	DEBUG_LOG(("sessionUserSerialized set: %1"
		).arg(selfSerialized.size()));

	_storedSessionSettings = std::move(data);
	_sessionUserSerialized = std::move(selfSerialized);
	_sessionUserStreamVersion = selfStreamVersion;
}

SessionSettings *Account::getSessionSettings() {
	if (_sessionUserId) {
		return _storedSessionSettings
			? _storedSessionSettings.get()
			: nullptr;
	} else if (const auto session = maybeSession()) {
		return &session->settings();
	}
	return nullptr;
}

void Account::setMtpAuthorization(const QByteArray &serialized) {
	Expects(!_mtp);

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto legacyUserId = Serialize::read<qint32>(stream);
	auto legacyMainDcId = Serialize::read<qint32>(stream);
	auto userId = quint64();
	auto mainDcId = qint32();
	if (((uint64(legacyUserId) << 32) | uint64(legacyMainDcId))
		== kWideIdsTag) {
		userId = Serialize::read<quint64>(stream);
		mainDcId = Serialize::read<qint32>(stream);
	} else {
		userId = legacyUserId;
		mainDcId = legacyMainDcId;
	}
	if (stream.status() != QDataStream::Ok) {
		LOG(("MTP Error: "
			"Could not read main fields from mtp authorization."));
		return;
	}

	setSessionUserId(userId);
	_mtpFields.mainDcId = mainDcId;

	const auto readKeys = [&](auto &keys) {
		const auto count = Serialize::read<qint32>(stream);
		if (stream.status() != QDataStream::Ok) {
			LOG(("MTP Error: "
				"Could not read keys count from mtp authorization."));
			return;
		}
		keys.reserve(count);
		for (auto i = 0; i != count; ++i) {
			const auto dcId = Serialize::read<qint32>(stream);
			const auto keyData = Serialize::read<MTP::AuthKey::Data>(stream);
			if (stream.status() != QDataStream::Ok) {
				LOG(("MTP Error: "
					"Could not read key from mtp authorization."));
				return;
			}
			keys.push_back(std::make_shared<MTP::AuthKey>(MTP::AuthKey::Type::ReadFromFile, dcId, keyData));
		}
	};
	readKeys(_mtpFields.keys);
	readKeys(_mtpKeysToDestroy);
	LOG(("MTP Info: "
		"read keys, current: %1, to destroy: %2"
		).arg(_mtpFields.keys.size()
		).arg(_mtpKeysToDestroy.size()));
}

void Account::startMtp(std::unique_ptr<MTP::Config> config) {
	Expects(!_mtp);

	auto fields = base::take(_mtpFields);
	fields.config = std::move(config);
	fields.network = _network;
	fields.deviceModel = Platform::DeviceModelPretty();
	fields.systemVersion = Platform::SystemVersionPretty();
	_mtp = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::Normal,
		std::move(fields));

	const auto writingKeys = _mtp->lifetime().make_state<bool>(false);
	_mtp->writeKeysRequests(
	) | rpl::filter([=] {
		return !*writingKeys;
	}) | rpl::on_next([=] {
		*writingKeys = true;
		Ui::PostponeCall(_mtp.get(), [=] {
			local().writeMtpData();
			*writingKeys = false;
		});
	}, _mtp->lifetime());

	const auto writingConfig = _lifetime.make_state<bool>(false);
	rpl::merge(
		_mtp->config().updates(),
		_mtp->dcOptions().changed() | rpl::to_empty
	) | rpl::filter([=] {
		return !*writingConfig;
	}) | rpl::on_next([=] {
		*writingConfig = true;
		Ui::PostponeCall(_mtp.get(), [=] {
			local().writeMtpConfig();
			*writingConfig = false;
		});
	}, _lifetime);

	_mtpFields.mainDcId = _mtp->mainDcId();

	_mtp->setUpdatesHandler([=](const MTP::Response &message) {
		checkForUpdates(message) || checkForNewSession(message);
	});
	_mtp->setGlobalFailHandler([=](const MTP::Error &, const MTP::Response &) {
		if (const auto session = maybeSession()) {
			crl::on_main(session, [=] { logOut(); });
		}
	});
	_mtp->setStateChangedHandler([=](MTP::ShiftedDcId dc, int32 state) {
		if (dc == _mtp->mainDcId()) {
			Core::App().settings().proxy().connectionTypeChangesNotify();
			if (_network->snapshot().state == MTP::NetworkState::Legacy) {
				Core::App().checkProxyRotation(this, state);
			} else if (state < 0
				&& _network->snapshot().state == MTP::NetworkState::Ready) {
				cancelProxyCheck();
				_network->block();
				scheduleProxyRetry();
			}
		}
	});
	_mtp->setSessionResetHandler([=](MTP::ShiftedDcId shiftedDcId) {
		if (const auto session = maybeSession()) {
			if (shiftedDcId == _mtp->mainDcId()) {
				session->updates().getDifference();
			}
		}
	});

	if (!_mtpKeysToDestroy.empty()) {
		destroyMtpKeys(base::take(_mtpKeysToDestroy));
	}

	if (_sessionUserId) {
		createSession(
			_sessionUserId,
			base::take(_sessionUserSerialized),
			base::take(_sessionUserStreamVersion),
			(_storedSessionSettings
				? std::move(_storedSessionSettings)
				: std::make_unique<SessionSettings>()));
	}
	_storedSessionSettings = nullptr;

	if (const auto session = maybeSession()) {
		// Skip all pending self updates so that we won't local().writeSelf.
		session->changes().sendNotifications();
	}

	_mtpValue = _mtp.get();
}

bool Account::checkForUpdates(const MTP::Response &message) {
	auto updates = MTPUpdates();
	auto from = message.reply.constData();
	if (!updates.read(from, from + message.reply.size())) {
		return false;
	}
	_mtpUpdates.fire(std::move(updates));
	return true;
}

bool Account::checkForNewSession(const MTP::Response &message) {
	auto newSession = MTPNewSession();
	auto from = message.reply.constData();
	if (!newSession.read(from, from + message.reply.size())) {
		return false;
	}
	_mtpNewSessionCreated.fire({});
	return true;
}

void Account::logOut() {
	if (_loggingOut) {
		return;
	}
	_loggingOut = true;
	if (_mtp) {
		_mtp->logout([=] { loggedOut(); });
	} else {
		// We log out because we've forgotten passcode.
		loggedOut();
	}
}

bool Account::loggingOut() const {
	return _loggingOut;
}

void Account::forcedLogOut() {
	if (sessionExists()) {
		loggedOut();
		resetAuthorizationKeys();
	}
}

void Account::loggedOut() {
	cancelProxyCheck();
	_network->block();
	_local->writePref<bool>("account-pending-login", false);
	_loggingOut = false;
	Media::Player::mixer()->stopAndClear();
	destroySession(DestroyReason::LoggedOut);
	local().reset();
	cSetOtherOnline(0);
}

void Account::destroyMtpKeys(MTP::AuthKeysList &&keys) {
	Expects(_mtp != nullptr);

	if (keys.empty()) {
		return;
	}
	if (_mtpForKeysDestroy) {
		_mtpForKeysDestroy->addKeysForDestroy(std::move(keys));
		local().writeMtpData();
		return;
	}
	auto destroyFields = MTP::Instance::Fields();

	destroyFields.network = _network;
	destroyFields.mainDcId = MTP::Instance::Fields::kNoneMainDc;
	destroyFields.config = std::make_unique<MTP::Config>(_mtp->config());
	destroyFields.keys = std::move(keys);
	destroyFields.deviceModel = Platform::DeviceModelPretty();
	destroyFields.systemVersion = Platform::SystemVersionPretty();
	_mtpForKeysDestroy = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::KeysDestroyer,
		std::move(destroyFields));
	_mtpForKeysDestroy->writeKeysRequests(
	) | rpl::on_next([=] {
		local().writeMtpData();
	}, _mtpForKeysDestroy->lifetime());
	_mtpForKeysDestroy->allKeysDestroyed(
	) | rpl::on_next([=] {
		LOG(("MTP Info: all keys scheduled for destroy are destroyed."));
		crl::on_main(this, [=] {
			_mtpForKeysDestroy = nullptr;
			local().writeMtpData();
		});
	}, _mtpForKeysDestroy->lifetime());
}

void Account::suggestMainDcId(MTP::DcId mainDcId) {
	Expects(_mtp != nullptr);

	_mtp->suggestMainDcId(mainDcId);
	if (_mtpFields.mainDcId != MTP::Instance::Fields::kNotSetMainDc) {
		_mtpFields.mainDcId = mainDcId;
	}
}

void Account::destroyStaleAuthorizationKeys() {
	Expects(_mtp != nullptr);

	for (const auto &key : _mtp->getKeysForWrite()) {
		// Disable this for now.
		if (key->type() == MTP::AuthKey::Type::ReadFromFile) {
			_mtpKeysToDestroy = _mtp->getKeysForWrite();
			LOG(("MTP Info: destroying stale keys, count: %1"
				).arg(_mtpKeysToDestroy.size()));
			resetAuthorizationKeys();
			return;
		}
	}
}

void Account::setHandleLoginCode(Fn<void(QString)> callback) {
	_handleLoginCode = std::move(callback);
}

void Account::handleLoginCode(const QString &code) const {
	if (_handleLoginCode) {
		_handleLoginCode(code);
	}
}

void Account::resetAuthorizationKeys() {
	Expects(_mtp != nullptr);

	cancelProxyCheck();

	{
		const auto old = base::take(_mtp);
		auto config = std::make_unique<MTP::Config>(old->config());
		startMtp(std::move(config));
	}
	if (_network->snapshot().state == MTP::NetworkState::Checking) {
		retryProxy();
	}
	local().writeMtpData();
}

} // namespace Main
