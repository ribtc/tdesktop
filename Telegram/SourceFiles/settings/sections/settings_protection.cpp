#include "settings/sections/settings_protection.h"

#include "api/api_authorizations.h"
#include "apiwrap.h"
#include "base/invoke_queued.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/components/retained_messages.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/account_management.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_protection.h"
#include "main/main_session.h"
#include "settings/sections/settings_active_sessions.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_settings.h"
#include "styles/style_window.h"

namespace Settings {
namespace {

class ProtectionPage final : public Ui::RpWidget {
public:
	ProtectionPage(QWidget *parent, not_null<Window::Controller*> controller);

private:
	void refresh();
	void scheduleRefresh();
	void layout();
	void selectGroup();
	void edit(Fn<void(Main::ProtectionSettings&)> change);
	void showTrusted();
	void showEvents();
	void showDevices(not_null<Main::Session*> session);
	void addToggle(
		rpl::producer<QString> title,
		rpl::producer<QString> description,
		bool Main::ProtectionSettings::*field);
	void addCaption(
		not_null<Ui::VerticalLayout*> parent,
		rpl::producer<QString> text);
	[[nodiscard]] std::vector<not_null<Main::Session*>> sessions() const;

	const not_null<Window::Controller*> _controller;
	Main::AccountMetadata _metadata;
	QString _group;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::LinkButton> _scope;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::RpWidget> _body;
	QPointer<Ui::VerticalLayout> _options;
	QPointer<Ui::VerticalLayout> _status;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::lifetime _accountsLifetime;
	bool _refreshScheduled = false;
	bool _layingOut = false;

};

ProtectionPage::ProtectionPage(
		QWidget *parent,
		not_null<Window::Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _title(this, tr::lng_protection_title(), st::protectionTitle)
, _scope(this, QString(), st::protectionLink)
, _scroll(this, st::defaultScrollArea) {
	if (!_metadata.load(Core::App().settings().readPref<QByteArray>(
			"account-management-metadata"))) {
		setDisabled(true);
	}
	if (const auto session = controller->maybeSession()) {
		_group = _metadata.group(QString::number(session->uniqueId()));
	}
	_scope->setClickedCallback([=] { selectGroup(); });
	Core::App().domain().accountsChanges() | rpl::on_next([=] {
		scheduleRefresh();
	}, lifetime());
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = Painter(this);
		p.fillRect(clip, st::windowBg);
		p.fillRect(
			0,
			st::protectionHeaderHeight - st::lineWidth,
			width(),
			st::lineWidth,
			st::shadowFg);
	}, lifetime());
	sizeValue() | rpl::on_next([=] { layout(); }, lifetime());
	refresh();
}

std::vector<not_null<Main::Session*>> ProtectionPage::sessions() const {
	auto result = std::vector<not_null<Main::Session*>>();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (session
			&& (_metadata.group(QString::number(session->uniqueId()))
				== _group)) {
			result.push_back(session);
		}
	}
	return result;
}

void ProtectionPage::scheduleRefresh() {
	if (_refreshScheduled) {
		return;
	}
	_refreshScheduled = true;
	InvokeQueued(this, [=] {
		_refreshScheduled = false;
		refresh();
	});
}

void ProtectionPage::addCaption(
		not_null<Ui::VerticalLayout*> parent,
		rpl::producer<QString> text) {
	parent->add(
		object_ptr<Ui::FlatLabel>(
			parent,
			std::move(text),
			st::protectionCaption),
		st::protectionCaptionPadding);
}

void ProtectionPage::addToggle(
		rpl::producer<QString> title,
		rpl::producer<QString> description,
		bool Main::ProtectionSettings::*field) {
	const auto selected = sessions();
	const auto checked = !selected.empty()
		&& selected.front()->protection().settings().*field;
	Ui::AddSkip(_options, st::protectionSectionSkip);
	const auto button = AddButtonWithIcon(
		_options,
		std::move(title),
		st::settingsButtonNoIcon);
	button->setDisabled(selected.empty());
	button->toggleOn(rpl::single(checked))->toggledChanges(
	) | rpl::on_next([=](bool value) {
		edit([=](Main::ProtectionSettings &settings) {
			settings.*field = value;
		});
	}, button->lifetime());
	addCaption(_options, std::move(description));
}

void ProtectionPage::refresh() {
	_accountsLifetime.destroy();
	const auto oldTop = _scroll->scrollTop();
	const auto selected = sessions();
	const auto groupName = _metadata.groups().value(
		_group,
		tr::lng_management_ungrouped(tr::now));
	_scope->setText(tr::lng_protection_scope(
		tr::now,
		lt_name,
		groupName,
		lt_amount,
		QString::number(selected.size())));
	_body = _scroll->setOwnedWidget(object_ptr<Ui::RpWidget>(this));
	_options = Ui::CreateChild<Ui::VerticalLayout>(_body);
	_status = Ui::CreateChild<Ui::VerticalLayout>(_body);
	_body->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = Painter(_body);
		p.fillRect(clip, st::windowBg);
		if (width() >= st::protectionWideWidth) {
			p.fillRect(
				_status->x(),
				0,
				st::lineWidth,
				_body->height(),
				st::shadowFg);
		}
	}, _body->lifetime());
	const auto settings = selected.empty()
		? Main::ProtectionSettings()
		: selected.front()->protection().settings();
	Ui::AddSubsectionTitle(_options, tr::lng_protection_heading());
	addCaption(_options, tr::lng_protection_about());
	const auto mixed = ranges::any_of(selected, [&](const auto session) {
		const auto &other = session->protection().settings();
		return other.retainDeleted != settings.retainDeleted
			|| other.unreadPreview != settings.unreadPreview
			|| other.guardLogins != settings.guardLogins
			|| other.terminateUnknown != settings.terminateUnknown
			|| other.retentionDays != settings.retentionDays;
	});
	const auto writable = ranges::all_of(selected, [](const auto session) {
		return session->protection().writable()
			&& session->retainedMessages().available();
	});
	addCaption(
		_options,
		!writable
			? tr::lng_protection_storage_error()
			: mixed
			? tr::lng_protection_mixed()
			: tr::lng_protection_saved());
	_options->setDisabled(!writable);
	addToggle(
		tr::lng_protection_logins(),
		tr::lng_protection_logins_about(),
		&Main::ProtectionSettings::guardLogins);
	const auto policy = std::make_shared<Ui::RadiobuttonGroup>(
		settings.terminateUnknown ? 1 : 0);
	for (auto i = 0; i != 2; ++i) {
		const auto radio = _options->add(
			object_ptr<Ui::Radiobutton>(
				_options,
				policy,
				i,
				i
					? tr::lng_protection_terminate(tr::now)
					: tr::lng_protection_notify(tr::now)),
			st::protectionCaptionPadding);
		radio->setDisabled(!settings.guardLogins || selected.empty());
	}
	policy->setChangedCallback([=](int value) {
		edit([=](Main::ProtectionSettings &settings) {
			settings.terminateUnknown = (value == 1);
		});
	});
	AddButtonWithIcon(
		_options,
		tr::lng_protection_trusted(),
		st::settingsButtonNoIcon)->setClickedCallback([=] { showTrusted(); });
	addCaption(_options, tr::lng_protection_login_limit());
	Ui::AddDivider(_options);
	addToggle(
		tr::lng_protection_recall(),
		tr::lng_protection_recall_about(),
		&Main::ProtectionSettings::retainDeleted);
	AddButtonWithLabel(
		_options,
		tr::lng_protection_retention(),
		tr::lng_protection_days(
			lt_amount,
			rpl::single(QString::number(settings.retentionDays))),
		st::settingsButtonNoIcon)->setClickedCallback([=] {
		_menu = base::make_unique_q<Ui::PopupMenu>(this);
		for (const auto days : { 7, 30, 90, 365 }) {
			_menu->addAction(
				tr::lng_protection_days(
					tr::now,
					lt_amount,
					QString::number(days)),
				[=] {
					edit([=](Main::ProtectionSettings &settings) {
						settings.retentionDays = days;
					});
				});
		}
		_menu->popup(QCursor::pos());
	});
	addCaption(_options, tr::lng_protection_media_about());
	Ui::AddDivider(_options);
	addToggle(
		tr::lng_protection_preview(),
		tr::lng_protection_preview_about(),
		&Main::ProtectionSettings::unreadPreview);
	addCaption(_options, tr::lng_protection_normal_open());
	AddButtonWithIcon(
		_options,
		tr::lng_protection_preview_help(),
		st::settingsButtonNoIcon)->setClickedCallback([=] {
		_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(tr::lng_protection_preview());
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_protection_preview_instructions(),
				st::defaultFlatLabel));
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	});
	Ui::AddSubsectionTitle(_status, tr::lng_protection_pending());
	auto pending = 0;
	auto events = std::vector<std::pair<QString, Main::ProtectionEvent>>();
	for (const auto session : selected) {
		session->protection().changes() | rpl::on_next([=] {
			scheduleRefresh();
		}, _accountsLifetime);
		session->account().network().changes() | rpl::on_next([=] {
			scheduleRefresh();
		}, _accountsLifetime);
		const auto &protection = session->protection();
		for (const auto &[hash, reason] : protection.pending()) {
			++pending;
			addCaption(_status, rpl::single(session->user()->name()));
			addCaption(_status, rpl::single(reason));
			const auto weak = base::make_weak(session);
			AddButtonWithIcon(
				_status,
				tr::lng_protection_devices(),
				st::settingsButtonNoIcon)->setClickedCallback([=] {
				if (const auto strong = weak.get()) {
					showDevices(strong);
				}
			});
		}
		if (protection.settings().guardLogins
			&& (session->account().network().snapshot().state
				!= MTP::NetworkState::Ready)) {
			++pending;
			addCaption(_status, rpl::single(session->user()->name()));
			addCaption(_status, tr::lng_protection_offline());
		}
		for (const auto &event : protection.events()) {
			events.emplace_back(session->user()->name(), event);
		}
	}
	if (!pending) {
		addCaption(_status, tr::lng_protection_no_pending());
	}
	Ui::AddDivider(_status);
	Ui::AddSubsectionTitle(_status, tr::lng_protection_recent());
	ranges::stable_sort(events, [](const auto &a, const auto &b) {
		return a.second.date > b.second.date;
	});
	for (auto i = 0; i < std::min(3, int(events.size())); ++i) {
		const auto &[name, event] = events[i];
		addCaption(
			_status,
			rpl::single(QLocale().toString(
				base::unixtime::parse(event.date).time(),
				QLocale::ShortFormat)));
		addCaption(_status, rpl::single(name + u" · "_q + event.text));
	}
	if (events.empty()) {
		addCaption(_status, tr::lng_protection_no_events());
	}
	AddButtonWithIcon(
		_status,
		tr::lng_protection_all_events(),
		st::settingsButtonNoIcon)->setClickedCallback([=] { showEvents(); });
	addCaption(_status, tr::lng_protection_group_only());
	_options->heightValue() | rpl::on_next([=] {
		layout();
	}, _options->lifetime());
	_status->heightValue() | rpl::on_next([=] {
		layout();
	}, _status->lifetime());
	layout();
	_scroll->scrollToY(oldTop);
}

void ProtectionPage::layout() {
	if (_layingOut) {
		return;
	}
	_layingOut = true;
	const auto guard = gsl::finally([&] { _layingOut = false; });
	const auto pad = st::protectionPadding;
	const auto header = st::protectionHeaderHeight;
	_title->move(pad, (header - _title->height()) / 2);
	_scope->resize(
		std::max(
			1,
			std::min(
				_scope->naturalWidth(),
				width() - _title->width() - 3 * pad)),
		_scope->height());
	_scope->move(
		width() - pad - _scope->width(),
		(header - _scope->height()) / 2);
	_scroll->setGeometry(0, header, width(), std::max(0, height() - header));
	if (!_body || !_options || !_status) {
		return;
	}
	const auto wide = width() >= st::protectionWideWidth;
	const auto left = std::max(
		1,
		wide
			? width() - st::protectionStatusWidth - 2 * pad
			: width() - 2 * pad);
	_options->resizeToWidth(left);
	_options->move(pad, st::protectionSectionSkip);
	_status->resizeToWidth(wide ? st::protectionStatusWidth : left);
	_status->move(
		wide ? pad + left : pad,
		wide
			? st::protectionSectionSkip
			: _options->y() + _options->height() + pad);
	_body->resize(
		width(),
		std::max(
			_options->y() + _options->height(),
			_status->y() + _status->height()) + pad);
}

void ProtectionPage::edit(Fn<void(Main::ProtectionSettings&)> change) {
	for (const auto session : sessions()) {
		auto settings = session->protection().settings();
		change(settings);
		session->protection().setSettings(settings);
	}
	scheduleRefresh();
}

void ProtectionPage::selectGroup() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	const auto add = [&](QString id, QString name) {
		_menu->addAction(name, [=] {
			_group = id;
			scheduleRefresh();
		});
	};
	add({}, tr::lng_management_ungrouped(tr::now));
	for (auto i = _metadata.groups().begin()
		; i != _metadata.groups().end()
		; ++i) {
		add(i.key(), i.value());
	}
	_menu->popup(QCursor::pos());
}

void FillTrustedDevices(
		not_null<Ui::VerticalLayout*> wrap,
		base::weak_ptr<Main::Session> weak,
		const Api::Authorizations::List &list) {
	wrap->clear();
	const auto session = weak.get();
	if (!session) {
		return;
	} else if (list.empty()) {
		wrap->add(object_ptr<Ui::FlatLabel>(
			wrap,
			tr::lng_protection_loading_devices(),
			st::protectionCaption));
	}
	for (const auto &entry : list) {
		const auto check = wrap->add(object_ptr<Ui::Checkbox>(
			wrap,
			entry.name + u" · "_q + entry.info,
			session->protection().trusted(entry.hash)));
		check->setDisabled(!entry.hash);
		check->checkedChanges() | rpl::on_next([=](bool trusted) {
			if (const auto strong = weak.get()) {
				strong->protection().trust(entry.hash, trusted);
			}
		}, check->lifetime());
	}
}

void AddTrustedAccount(
		not_null<Ui::GenericBox*> box,
		base::weak_ptr<Main::Session> weak) {
	const auto session = weak.get();
	if (!session) {
		return;
	}
	Ui::AddSubsectionTitle(
		box->verticalLayout(),
		rpl::single(session->user()->name()));
	const auto wrap = box->addRow(object_ptr<Ui::VerticalLayout>(box));
	session->api().authorizations().listValue(
	) | rpl::on_next([=](const auto &list) {
		FillTrustedDevices(wrap, weak, list);
	}, wrap->lifetime());
	session->api().authorizations().reload();
}

void ProtectionPage::showTrusted() {
	auto selected = std::vector<base::weak_ptr<Main::Session>>();
	for (const auto session : sessions()) {
		selected.push_back(base::make_weak(session));
	}
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_protection_trusted());
		box->setWidth(st::protectionDialogWidth);
		for (const auto &weak : selected) {
			AddTrustedAccount(box, weak);
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ProtectionPage::showEvents() {
	auto text = QString();
	for (const auto session : sessions()) {
		for (const auto &event : session->protection().events()) {
			text += QLocale().toString(
					base::unixtime::parse(event.date),
					QLocale::ShortFormat)
				+ u" · "_q
				+ session->user()->name()
				+ u"\n"_q
				+ event.text
				+ u"\n\n"_q;
		}
	}
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_protection_all_events());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			text.isEmpty() ? tr::lng_protection_no_events(tr::now) : text,
			st::defaultFlatLabel));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ProtectionPage::showDevices(not_null<Main::Session*> session) {
	const auto controller = _controller;
	const auto account = &session->account();
	controller->widget()->closeAccountManagement();
	Core::App().domain().maybeActivate(account);
	if (const auto active = controller->sessionController()) {
		active->showSettings(SessionsId());
	}
}

} // namespace

object_ptr<Ui::RpWidget> CreateProtection(
		QWidget *parent,
		not_null<Window::Controller*> controller) {
	return object_ptr<ProtectionPage>(parent, controller);
}

} // namespace Settings
