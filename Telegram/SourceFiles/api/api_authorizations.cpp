/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_authorizations.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/changelogs.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "data/data_session.h"
#include "main/main_protection.h"
#include "main/main_app_config.h"
#include "main/main_session_settings.h"
#include "main/main_session.h"

namespace Api {
namespace {

constexpr auto TestApiId = 17349;
constexpr auto SnapApiId = 611335;
constexpr auto DesktopApiId = 2040;

Authorizations::Entry ParseEntry(const MTPDauthorization &data) {
	auto result = Authorizations::Entry();

	result.hash = data.is_current() ? 0 : data.vhash().v;
	result.incomplete = data.is_password_pending();
	result.createdTime = data.vdate_created().v;
	result.unconfirmed = data.is_unconfirmed();
	result.callsDisabled = data.is_call_requests_disabled();

	const auto apiId = result.apiId = data.vapi_id().v;
	const auto isTest = (apiId == TestApiId);
	const auto isDesktop = (apiId == DesktopApiId)
		|| (apiId == SnapApiId)
		|| isTest;

	const auto appName = isDesktop
		? u"Telegram Desktop%1"_q.arg(isTest ? " (GitHub)" : QString())
		: qs(data.vapp_name());// + u" for "_q + qs(d.vplatform());
	const auto appVer = [&] {
		const auto version = qs(data.vapp_version());
		if (isDesktop) {
			const auto verInt = version.toInt();
			if (version == QString::number(verInt)) {
				return Core::FormatVersionDisplay(verInt);
			}
		} else {
			if (const auto index = version.indexOf('('); index >= 0) {
				return version.mid(index);
			}
		}
		return version;
	}();

	result.name = result.hash
		? qs(data.vdevice_model())
		: Core::App().settings().deviceModel();

	const auto country = qs(data.vcountry());
	//const auto platform = qs(data.vplatform());
	//const auto &countries = countriesByISO2();
	//const auto j = countries.constFind(country);
	//if (j != countries.cend()) {
	//	country = QString::fromUtf8(j.value()->name);
	//}
	result.system = qs(data.vsystem_version());
	result.platform = qs(data.vplatform());
	result.activeTime = data.vdate_active().v
		? data.vdate_active().v
		: data.vdate_created().v;
	result.info = QString("%1%2").arg(
		appName,
		appVer.isEmpty() ? QString() : (' ' + appVer));
	result.ip = qs(data.vip());
	result.active = result.hash
		? Authorizations::ActiveDateString(result.activeTime)
		: tr::lng_status_online(tr::now);
	result.location = country;

	return result;
}

} // namespace

Authorizations::Authorizations(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance())
, _protectionTimer([=] {
	if (_session->protection().settings().guardLogins) {
		reload();
	}
})
, _autoconfirmPeriod([=] {
	constexpr auto kFallbackCount = 604800;
	return api->session().appConfig().get<int>(
		u"authorization_autoconfirm_period"_q,
		kFallbackCount);
})
, _saveUnreviewed([=] {
	api->session().settings().setUnreviewed(_unreviewed);
	api->session().saveSettingsDelayed();
}) {
	crl::on_main(_session, [=] {
		_session->protection().settingsChanges() | rpl::on_next([=] {
			reconcileProtection();
		}, _lifetime);
		if (_session->protection().settings().guardLogins) {
			reload();
		}
	});
	_unreviewed = api->session().settings().unreviewed();
	crl::on_main(&api->session(), [=] { removeExpiredUnreviewed(); });
	Core::App().settings().deviceModelChanges(
	) | rpl::on_next([=](const QString &model) {
		auto changed = false;
		for (auto &entry : _list) {
			if (!entry.hash) {
				entry.name = model;
				changed = true;
			}
		}
		if (changed) {
			_listChanges.fire({});
		}
	}, _lifetime);

	if (Core::App().settings().disableCallsLegacy()) {
		toggleCallsDisabledHere(true);
	}
}

void Authorizations::reload() {
	if (_session->protection().settings().guardLogins) {
		_protectionTimer.callOnce(60 * 1000);
	}
	if (_requestId) {
		return;
	}

	_requestId = _api.request(MTPaccount_GetAuthorizations(
	)).done([=](const MTPaccount_Authorizations &result) {
		_requestId = 0;
		_lastReceived = crl::now();
		const auto &data = result.data();
		_ttlDays = data.vauthorization_ttl_days().v;
		_list = ranges::views::all(
			data.vauthorizations().v
		) | ranges::views::transform([](const MTPAuthorization &auth) {
			return ParseEntry(auth.data());
		}) | ranges::to<List>;
		removeExpiredUnreviewed();
		refreshCallsDisabledHereFromCloud();
		guardAuthorizations();
		_listChanges.fire({});
	}).fail([=] {
		_requestId = 0;
	}).send();
}

void Authorizations::cancelCurrentRequest() {
	_api.request(base::take(_requestId)).cancel();
}

void Authorizations::refreshCallsDisabledHereFromCloud() {
	const auto that = ranges::find(_list, 0, &Entry::hash);
	if (that != end(_list)
		&& !_toggleCallsDisabledRequests.contains(0)) {
		_callsDisabledHere = that->callsDisabled;
	}
}

void Authorizations::requestTerminate(
		Fn<void(const MTPBool &result)> &&done,
		Fn<void(const MTP::Error &error)> &&fail,
		std::optional<uint64> hash) {
	const auto send = [&](auto request) {
		_api.request(
			std::move(request)
		).done([=, done = std::move(done)](const MTPBool &result) {
			done(result);
			if (mtpIsTrue(result)) {
				if (hash) {
					_session->protection().resolve(*hash);
					_list.erase(
						ranges::remove(_list, *hash, &Entry::hash),
						end(_list));
				} else {
					_list.clear();
				}
				_listChanges.fire({});
			}
		}).fail(
			std::move(fail)
		).send();
	};
	if (hash) {
		send(MTPaccount_ResetAuthorization(MTP_long(*hash)));
	} else {
		send(MTPauth_ResetAuthorizations());
	}
}

Authorizations::List Authorizations::list() const {
	return _list;
}

auto Authorizations::listValue() const
-> rpl::producer<Authorizations::List> {
	return rpl::single(
		list()
	) | rpl::then(
		_listChanges.events() | rpl::map([=] { return list(); })
	);
}

rpl::producer<int> Authorizations::totalValue() const {
	return rpl::single(
		total()
	) | rpl::then(
		_listChanges.events() | rpl::map([=] { return total(); })
	);
}

void Authorizations::updateTTL(int days) {
	_api.request(_ttlRequestId).cancel();
	_ttlRequestId = _api.request(MTPaccount_SetAuthorizationTTL(
		MTP_int(days)
	)).done([=] {
		_ttlRequestId = 0;
	}).fail([=] {
		_ttlRequestId = 0;
	}).send();
	_ttlDays = days;
}

rpl::producer<int> Authorizations::ttlDays() const {
	return _ttlDays.value() | rpl::filter(rpl::mappers::_1 != 0);
}

void Authorizations::toggleCallsDisabled(uint64 hash, bool disabled) {
	if (const auto sent = _toggleCallsDisabledRequests.take(hash)) {
		_api.request(*sent).cancel();
	}
	using Flag = MTPaccount_ChangeAuthorizationSettings::Flag;
	const auto id = _api.request(MTPaccount_ChangeAuthorizationSettings(
		MTP_flags(Flag::f_call_requests_disabled),
		MTP_long(hash),
		MTPBool(), // encrypted_requests_disabled
		MTP_bool(disabled)
	)).done([=] {
		_toggleCallsDisabledRequests.remove(hash);
	}).fail([=] {
		_toggleCallsDisabledRequests.remove(hash);
	}).send();
	_toggleCallsDisabledRequests.emplace(hash, id);
	if (!hash) {
		_callsDisabledHere = disabled;
	}
}

bool Authorizations::callsDisabledHere() const {
	return _callsDisabledHere.current();
}

rpl::producer<bool> Authorizations::callsDisabledHereValue() const {
	return _callsDisabledHere.value();
}

QString Authorizations::ActiveDateString(TimeId active) {
	const auto now = QDateTime::currentDateTime();
	const auto lastTime = base::unixtime::parse(active);
	const auto nowDate = now.date();
	const auto lastDate = lastTime.date();
	return (lastDate == nowDate)
		? QLocale().toString(lastTime.time(), QLocale::ShortFormat)
		: (lastDate.year() == nowDate.year()
			&& lastDate.weekNumber() == nowDate.weekNumber())
		? langDayOfWeek(lastDate)
		: QLocale().toString(lastDate, QLocale::ShortFormat);
}

int Authorizations::total() const {
	return ranges::count_if(
		_list,
		ranges::not_fn(&Entry::incomplete));
}

crl::time Authorizations::lastReceivedTime() {
	return _lastReceived;
}

const std::vector<Data::UnreviewedAuth> &Authorizations::unreviewed() {
	removeExpiredUnreviewed();
	return _unreviewed;
}

void Authorizations::removeExpiredUnreviewed() {
	const auto now = base::unixtime::now();
	const auto period = _autoconfirmPeriod();

	const auto oldSize = _unreviewed.size();
	_unreviewed.erase(
		std::remove_if(_unreviewed.begin(), _unreviewed.end(),
			[=](const auto &auth) {
				return (now - auth.date) >= period;
			}),
		_unreviewed.end());

	if (_unreviewed.size() != oldSize) {
		_saveUnreviewed();
	}
}

void Authorizations::review(const std::vector<uint64> &hashes, bool confirm) {
	for (const auto hash : hashes) {
		if (const auto sent = _reviewRequests.take(hash)) {
			_api.request(*sent).cancel();
		}
	}

	const auto checkComplete = [=] {
		if (_reviewRequests.empty()) {
			_saveUnreviewed();
			_unreviewedChanges.fire({});
		}
	};

	for (const auto hash : hashes) {
		const auto removeFromUnreviewed = [=] {
			_unreviewed.erase(
				std::remove_if(_unreviewed.begin(), _unreviewed.end(),
					[hash](const auto &auth) { return auth.hash == hash; }),
				_unreviewed.end());
			_reviewRequests.remove(hash);
			checkComplete();
		};

		if (confirm) {
			using Flag = MTPaccount_ChangeAuthorizationSettings::Flag;
			const auto id = _api.request(MTPaccount_ChangeAuthorizationSettings(
				MTP_flags(Flag::f_confirmed),
				MTP_long(hash),
				MTPBool(), // encrypted_requests_disabled
				MTPBool() // call_requests_disabled
			)).done([=] {
				removeFromUnreviewed();
			}).fail([=] {
				removeFromUnreviewed();
			}).send();
			_reviewRequests.emplace(hash, id);
		} else {
			const auto id = _api.request(MTPaccount_ResetAuthorization(
				MTP_long(hash)
			)).done([=](const MTPBool &result) {
				if (mtpIsTrue(result)) {
					_list.erase(
						ranges::remove(_list, hash, &Entry::hash),
						end(_list));
					_listChanges.fire({});
				}
				removeFromUnreviewed();
			}).fail([=] {
				removeFromUnreviewed();
			}).send();
			_reviewRequests.emplace(hash, id);
		}
	}
}

rpl::producer<> Authorizations::unreviewedChanges() const {
	return _unreviewedChanges.events();
}

void Authorizations::apply(const MTPUpdate &update) {
	if (_session->protection().settings().guardLogins) {
		reload();
	}
	removeExpiredUnreviewed();
	update.match([&](const MTPDupdateNewAuthorization &data) {
		auto unreviewed = Data::UnreviewedAuth{
			.hash = data.vhash().v,
			.unconfirmed = data.is_unconfirmed(),
			.date = data.vdate().value_or_empty(),
			.device = qs(data.vdevice().value_or_empty()),
			.location = qs(data.vlocation().value_or_empty())
		};
		if (!unreviewed.unconfirmed) {
			const auto hash = unreviewed.hash;
			const auto was = _unreviewed.size();
			_unreviewed.erase(
				std::remove_if(
					_unreviewed.begin(),
					_unreviewed.end(),
					[hash](const auto &auth) { return auth.hash == hash; }),
				_unreviewed.end());
			if (was != _unreviewed.size()) {
				_saveUnreviewed();
				_unreviewedChanges.fire({});
			}
			return;
		}

		for (auto &auth : _unreviewed) {
			if (auth.hash == unreviewed.hash) {
				auth = std::move(unreviewed);
				_saveUnreviewed();
				_unreviewedChanges.fire({});
				return;
			}
		}

		_unreviewed.push_back(std::move(unreviewed));
		_saveUnreviewed();
		_unreviewedChanges.fire({});
	}, [](auto&&) {
		Unexpected("Update in Authorizations::apply.");
	});
}

void Authorizations::guardAuthorizations() {
	auto &protection = _session->protection();
	const auto settings = protection.settings();
	auto resolved = std::vector<uint64>();
	for (const auto &[hash, reason] : protection.pending()) {
		if (ranges::none_of(_list, [=](const Entry &entry) {
			return entry.hash == hash;
		})) {
			resolved.push_back(hash);
		}
	}
	for (const auto hash : resolved) {
		protection.resolve(hash);
	}
	if (!settings.guardLogins) {
		return;
	}
	for (const auto &entry : _list) {
		const auto hash = entry.hash;
		if (protection.trusted(hash) || entry.incomplete) {
			continue;
		}
		if (entry.createdTime < settings.loginSince && !entry.unconfirmed) {
			continue;
		}
		if (!protection.pending().contains(hash)) {
			const auto text = tr::lng_protection_login_detected(
				tr::now,
				lt_name,
				entry.name);
			protection.setPending(hash, text);
			protection.record(text);
			_session->data().serviceNotification(tr::marked(text));
		}
		if (!settings.terminateUnknown || _protectionRequests.contains(hash)
			|| !_protectionAttempts.emplace(hash).second) {
			continue;
		}
		const auto device = entry.name;
		const auto requestId = _api.request(MTPaccount_ResetAuthorization(
			MTP_long(hash)
		)).done([=](const MTPBool &result) {
			finishProtection(hash, device, mtpIsTrue(result));
		}).fail([=](const MTP::Error &) {
			finishProtection(hash, device, false);
		}).send();
		_protectionRequests.emplace(hash, requestId);
	}
}

void Authorizations::reconcileProtection() {
	const auto &protection = _session->protection();
	const auto settings = protection.settings();
	for (auto i = _protectionRequests.begin(); i != _protectionRequests.end();) {
		if (!settings.guardLogins
			|| !settings.terminateUnknown
			|| protection.trusted(i->first)) {
			_api.request(i->second).cancel();
			i = _protectionRequests.erase(i);
		} else {
			++i;
		}
	}
	_protectionAttempts.clear();
	if (settings.guardLogins) {
		reload();
	} else {
		_protectionTimer.cancel();
	}
}

void Authorizations::finishProtection(
		uint64 hash,
		const QString &device,
		bool success) {
	if (!_protectionRequests.remove(hash)) {
		return;
	}
	auto &protection = _session->protection();
	if (!protection.settings().guardLogins
		|| !protection.settings().terminateUnknown
		|| protection.trusted(hash)) {
		return;
	} else if (!success) {
		reportProtectionFailure(hash, device);
		return;
	}
	protection.resolve(hash);
	protection.record(tr::lng_protection_login_terminated(tr::now, lt_name, device));
	_list.erase(ranges::remove(_list, hash, &Entry::hash), end(_list));
	_listChanges.fire({});
}

void Authorizations::reportProtectionFailure(
		uint64 hash,
		const QString &device) {
	const auto text = tr::lng_protection_login_failed(
		tr::now,
		lt_name,
		device);
	auto &protection = _session->protection();
	protection.setPending(hash, text);
	protection.record(text);
	_session->data().serviceNotification(tr::marked(text));
}

} // namespace Api
