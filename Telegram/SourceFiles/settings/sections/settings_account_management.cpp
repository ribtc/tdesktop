#include "settings/sections/settings_account_management.h"

#include "base/invoke_queued.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/proxy_library.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "intro/session_import_box.h"
#include "lang/lang_keys.h"
#include "main/account_management.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "storage/localstorage.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "window/window_controller.h"

#include "styles/style_layers.h"
#include "styles/style_window.h"

namespace Settings {
namespace {

constexpr auto kMetadataPref = "account-management-metadata";

QString Identity(not_null<Main::Account*> account) {
	const auto session = account->maybeSession();
	return session ? QString::number(session->uniqueId()) : QString();
}

Main::Account *FindAccount(const QString &id) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (Identity(entry.account.get()) == id) {
			return entry.account.get();
		}
	}
	return nullptr;
}

Main::AccountStatus Status(not_null<Main::Account*> account) {
	switch (account->network().snapshot().state) {
	case MTP::NetworkState::Ready: return Main::AccountStatus::Ready;
	case MTP::NetworkState::Checking: return Main::AccountStatus::Checking;
	case MTP::NetworkState::Failed: return Main::AccountStatus::Failed;
	default: return account->network().snapshot().allowed()
		? Main::AccountStatus::Direct : Main::AccountStatus::Unbound;
	}
}

QString StatusText(Main::AccountStatus status) {
	switch (status) {
	case Main::AccountStatus::Ready: return tr::lng_management_ready(tr::now);
	case Main::AccountStatus::Checking: return tr::lng_management_checking(tr::now);
	case Main::AccountStatus::Failed: return tr::lng_management_failed(tr::now);
	case Main::AccountStatus::Unbound: return tr::lng_management_unbound(tr::now);
	case Main::AccountStatus::Direct: return tr::lng_management_direct(tr::now);
	}
	Unexpected("Account status");
}

struct AccountTarget {
	QString id;
	QString name;
	base::weak_ptr<Main::Account> account;
};

std::vector<AccountTarget> FreezeTargets(const QStringList &ids) {
	auto result = std::vector<AccountTarget>();
	for (const auto &id : ids) {
		const auto account = FindAccount(id);
		result.push_back({ id,
			account ? account->session().user()->name() : id,
			account ? base::make_weak(account) : nullptr });
	}
	return result;
}

QString BindingResultText(const AccountTarget &target, uint64 operation) {
	const auto account = target.account.get();
	const auto valid = operation
		&& account
		&& Identity(account) == target.id;
	const auto changed = valid
		&& account->proxyBindingOperation() != operation;
	const auto status = changed ? tr::lng_management_replaced(tr::now)
		: valid ? StatusText(Status(account))
		: tr::lng_management_rejected(tr::now);
	return target.name + u": "_q + status;
}

void BindingResults(
		not_null<Window::Controller*> controller,
		std::vector<AccountTarget> targets,
		std::vector<uint64> operations) {
	Expects(targets.size() == operations.size());

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_management_results());
		for (auto i = 0; i != int(targets.size()); ++i) {
			const auto target = targets[i];
			const auto operation = operations[i];
			const auto initial = BindingResultText(target, operation);
			const auto label = box->addRow(
				object_ptr<Ui::FlatLabel>(box, initial, st::defaultFlatLabel),
				style::margins(
					st::boxRowPadding.left(),
					i ? 0 : st::boxRowPadding.top(),
					st::boxRowPadding.right(),
					(i + 1 == targets.size()) ? st::boxRowPadding.bottom() : 0),
				style::al_justify);
			const auto text = label->lifetime().make_state<QString>(initial);
			const auto update = [=] {
				const auto next = BindingResultText(target, operation);
				if (*text != next) {
					*text = next;
					label->setText(next);
				}
			};
			if (const auto account = target.account.get()) {
				account->network().changes(
				) | rpl::on_next(update, label->lifetime());
				account->sessionChanges(
				) | rpl::on_next(update, label->lifetime());
			}
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

class AccountRow final : public Ui::RpWidget {
public:
	AccountRow(
		QWidget *parent,
		not_null<Main::Account*> account,
		const Main::AccountMetadata &metadata,
		int number,
		bool selected,
		Fn<void()> toggle,
		Fn<void()> open,
		Fn<void()> bind,
		Fn<void()> actions);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	base::weak_ptr<Main::Account> _account;
	int _number = 0;
	QString _group;
	QString _tags;
	Ui::PeerUserpicView _userpic;
	bool _selected = false;

};

class AccountManagement final : public Ui::RpWidget {
public:
	AccountManagement(QWidget *parent, not_null<Window::Controller*> controller);

private:
	void scheduleRefresh();
	void refresh();
	void rebuildRows();
	void layout();
	void save();
	void groupMenu(bool assign);
	void showTagFilterMenu();
	void showStatusFilterMenu();
	void moveToGroup(QString group, const std::vector<AccountTarget> &targets);
	bool readMetadata();
	void editGroup(QString id = {});
	void rowMenu(QString id);
	void editTags(QStringList ids);
	void openAccount(QString id);
	void logoutAccount(QString id);
	void bindMenu(QStringList ids);
	void confirmBinding(QString proxy, QStringList ids);
	void editText(QString title, QString value, Fn<bool(QString)> save);
	Ui::RoundButton *button(
		QString text,
		Fn<void()> callback,
		const style::RoundButton &st = st::managementButton);

	const not_null<Window::Controller*> _controller;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _subtitle;
	object_ptr<Ui::RpWidget> _header;
	Ui::Checkbox *_selectPage = nullptr;
	object_ptr<Ui::InputField> _number;
	object_ptr<Ui::InputField> _search;
	object_ptr<Ui::ScrollArea> _scroll;
	object_ptr<Ui::RpWidget> _rows = { nullptr };
	object_ptr<Ui::FlatLabel> _summary;
	object_ptr<Ui::FlatLabel> _pageNumber;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Ui::RoundButton *_tagFilter = nullptr;
	Ui::RoundButton *_statusFilter = nullptr;
	Ui::RoundButton *_add = nullptr;
	Ui::RoundButton *_import = nullptr;
	Ui::RoundButton *_selectAll = nullptr;
	Ui::RoundButton *_clearSelection = nullptr;
	Ui::RoundButton *_tagsSelection = nullptr;
	Ui::RoundButton *_selectionMenu = nullptr;
	Ui::RoundButton *_moveSelection = nullptr;
	Ui::RoundButton *_bindSelection = nullptr;
	std::vector<Ui::RoundButton*> _paging;
	std::vector<Main::ManagedAccount> _accounts;
	Main::AccountMetadata _metadata;
	Main::AccountQuery _query;
	Main::AccountPage _page;
	Main::AccountSelection _selection;
	rpl::lifetime _accountsLifetime;
	bool _scheduled = false;
	bool _writable = true;

};

AccountRow::AccountRow(
	QWidget *parent,
	not_null<Main::Account*> account,
	const Main::AccountMetadata &metadata,
	int number,
	bool selected,
	Fn<void()> toggle,
	Fn<void()> open,
	Fn<void()> bind,
	Fn<void()> actions)
: RpWidget(parent)
, _account(base::make_weak(account.get()))
, _number(number)
, _group(metadata.groups().value(metadata.group(Identity(account))))
, _tags(metadata.tags(Identity(account)).join(u", "_q))
, _selected(selected) {
	resize(width(), st::accountLibraryRowHeight);
	const auto binding = Core::App().settings().proxyLibrary().find(account->proxyBindingId());
	setToolTip(QString::number(_number) + u" · "_q + _group
		+ u"\n"_q + _tags + u"\n"_q
		+ (binding ? binding->proxy.host : (Status(account) == Main::AccountStatus::Direct
			? tr::lng_management_direct(tr::now)
			: tr::lng_management_status_unbound(tr::now)))
		+ u"\n"_q + StatusText(Status(account)));
	const auto check = Ui::CreateChild<Ui::Checkbox>(this, QString(), selected,
		st::managementTableCheckbox, st::managementTableCheck);
	check->setAccessibleName(account->session().user()->name());
	check->checkedChanges() | rpl::on_next([=](bool checked) {
		_selected = checked;
		toggle();
		update();
	}, lifetime());
	const auto menu = Ui::CreateChild<Ui::LinkButton>(this, u"⋯"_q, st::managementLink);
	menu->setAccessibleName(tr::lng_management_actions(tr::now));
	menu->setClickedCallback(std::move(actions));
	const auto activate = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_management_activate(tr::now),
		st::managementLink);
	activate->setClickedCallback(std::move(open));
	const auto proxy = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_management_proxy_action(tr::now),
		st::managementLink);
	proxy->setClickedCallback(std::move(bind));
	sizeValue() | rpl::on_next([=](QSize size) {
		const auto pad = st::managementTablePadding;
		check->move(pad, (size.height() - check->height()) / 2);
		menu->move(
			size.width() - pad - menu->width(),
			(size.height() - menu->height()) / 2);
		const auto wide = size.width() >= st::accountLibraryWideTable;
		activate->setVisible(wide);
		proxy->setVisible(wide);
		activate->move(
			menu->x() - st::managementGap - activate->width(),
			(size.height() - activate->height()) / 2);
		proxy->move(
			activate->x() - st::managementGap - proxy->width(),
			(size.height() - proxy->height()) / 2);
	}, lifetime());
}

void AccountRow::paintEvent(QPaintEvent *e) {
	const auto account = _account.get();
	const auto session = account ? account->maybeSession() : nullptr;
	if (!session) {
		return;
	}
	auto p = Painter(this);
	p.fillRect(e->rect(), _selected ? st::lightButtonBgOver : st::windowBg);
	const auto pad = st::managementTablePadding;
	const auto size = st::managementAvatarSize;
	const auto wide = width() >= st::accountLibraryWideTable;
	const auto numberLeft = pad + st::managementCheckWidth + st::managementGap;
	const auto left = numberLeft + st::accountLibraryNumberWidth;
	const auto actionsLeft = width() - pad - st::accountLibraryActionsWidth;
	const auto proxyLeft = actionsLeft - st::accountLibraryProxyWidth;
	const auto tagsLeft = proxyLeft - st::accountLibraryTagsWidth;
	const auto usernameLeft = tagsLeft - st::accountLibraryUsernameWidth;
	const auto textRight = wide ? usernameLeft : width() - st::managementActionWidth;
	const auto showUserpic = textRight - left - pad >= 2 * (size + st::managementGap);
	if (showUserpic) {
		session->user()->paintUserpicLeft(
			p,
			_userpic,
			left,
			(height() - size) / 2,
			width(),
			size);
	}
	const auto textLeft = left + (showUserpic ? size + st::managementGap : 0);
	const auto textWidth = std::max(
		0,
		textRight - textLeft - pad);
	const auto top = (height() - (wide ? 2 : 3) * st::managementTableFont->height) / 2;
	p.setFont(st::managementNameFont);
	p.setPen(st::windowFg);
	p.drawTextLeft(
		textLeft,
		top,
		width(),
		st::managementNameFont->elided(session->user()->name(), textWidth));
	p.setFont(st::managementTableFont);
	p.setPen(st::windowSubTextFg);
	const auto details = session->user()->phone();
	p.drawTextLeft(
		textLeft,
		top + st::managementTableFont->height,
		width(),
		st::managementTableFont->elided(details, textWidth));
	const auto y = (height() - st::managementTableFont->height) / 2;
	p.drawTextLeft(
		numberLeft,
		y,
		width(),
		st::managementTableFont->elided(
			QString::number(_number),
			st::accountLibraryNumberWidth - pad));
	const auto labels = _group.isEmpty() ? _tags
		: _tags.isEmpty() ? _group : _group + u", "_q + _tags;
	if (!wide) {
		const auto proxy = Core::App().settings().proxyLibrary().find(account->proxyBindingId());
		const auto route = proxy ? proxy->proxy.host : (Status(account) == Main::AccountStatus::Direct
			? tr::lng_management_direct(tr::now)
			: tr::lng_management_status_unbound(tr::now));
		p.drawTextLeft(
			textLeft,
			top + 2 * st::managementTableFont->height,
			width(),
			st::managementTableFont->elided(route + u" · "_q + labels, textWidth));
	}
	if (wide) {
		const auto username = session->user()->username();
		p.drawTextLeft(
			usernameLeft,
			y,
			width(),
			st::managementTableFont->elided(
				username.isEmpty() ? u"—"_q : u"@"_q + username,
				st::accountLibraryUsernameWidth - pad));
		p.drawTextLeft(
			tagsLeft,
			y,
			width(),
			st::managementTableFont->elided(
				labels.isEmpty() ? u"—"_q : labels,
				st::accountLibraryTagsWidth - pad));
		const auto proxy = Core::App().settings().proxyLibrary().find(account->proxyBindingId());
		const auto label = proxy ? proxy->proxy.host
			: (Status(account) == Main::AccountStatus::Direct
			? tr::lng_management_direct(tr::now)
			: tr::lng_management_status_unbound(tr::now));
		p.setPen(proxy ? st::windowActiveTextFg
			: Status(account) == Main::AccountStatus::Direct ? st::windowSubTextFg
			: st::attentionButtonFg);
		p.drawTextLeft(
			proxyLeft,
			y,
			width(),
			st::managementTableFont->elided(label, st::accountLibraryProxyWidth - pad));
	}
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
	p.fillRect(0, 0, st::lineWidth, height(), st::shadowFg);
	p.fillRect(width() - st::lineWidth, 0, st::lineWidth, height(), st::shadowFg);
}

AccountManagement::AccountManagement(
	QWidget *parent,
	not_null<Window::Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _title(this, tr::lng_management_library(), st::managementTitle)
, _subtitle(this, QString(), st::managementCaption)
, _header(this)
, _number(this, st::accountLibraryNumberSearch, tr::lng_management_number())
, _search(this, st::managementSearch, tr::lng_management_search())
, _scroll(this, st::defaultScrollArea)
, _summary(this, QString(), st::managementCaption)
, _pageNumber(this, QString(), st::managementCaption) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusProxy(_search);
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(this);
		p.fillRect(clip, st::windowBg);
		p.fillRect(
			0,
			st::accountLibraryHeaderHeight - st::lineWidth,
			width(),
			st::lineWidth,
			st::shadowFg);
	}, lifetime());
	_add = button(tr::lng_management_add(tr::now), [=] {
		const auto environment = _controller->account().mtp().isTestMode()
			? MTP::Environment::Test : MTP::Environment::Production;
		Core::App().domain().addActivated(environment);
	}, st::managementPrimaryButton);
	_import = button(tr::lng_management_import(tr::now), [=] {
		Intro::ChooseSessionFiles(
			this,
			&_controller->account(),
			_controller->uiShow());
	});
	_tagFilter = button(tr::lng_management_filter_tags(tr::now), [=] {
		showTagFilterMenu();
	});
	_statusFilter = button(tr::lng_management_filter_status(tr::now), [=] {
		showStatusFilterMenu();
	});
	_selectAll = button(tr::lng_management_select_all(tr::now), [=] {
		_selection.select(_page.matching);
		rebuildRows();
	});
	_clearSelection = button(tr::lng_management_clear(tr::now), [=] {
		_selection.clear();
		rebuildRows();
	});
	_tagsSelection = button(tr::lng_management_tags_column(tr::now), [=] {
		editTags(_selection.freeze());
	});
	_moveSelection = button(tr::lng_management_move(tr::now), [=] {
		groupMenu(true);
	});
	_bindSelection = button(tr::lng_management_bind(tr::now), [=] {
		bindMenu(_selection.freeze());
	});
	_selectionMenu = button(tr::lng_management_actions(tr::now), [=] {
		_menu = base::make_unique_q<Ui::PopupMenu>(this);
		for (const auto tool : { _tagsSelection, _moveSelection,
				_bindSelection, _selectAll, _clearSelection }) {
			if (!tool->isDisabled()) {
				_menu->addAction(tool->accessibilityName(), [=] {
					Ui::PostponeCall(this, [=] {
						tool->clicked(Qt::NoModifier, Qt::LeftButton);
					});
				});
			}
		}
		_menu->addAction(tr::lng_management_groups(tr::now), [=] {
			Ui::PostponeCall(this, [=] { groupMenu(false); });
		});
		_menu->popup(QCursor::pos());
	});
	_paging.push_back(button(u"‹"_q, [=] {
		--_query.page;
		rebuildRows();
	}));
	_paging.push_back(button(u"›"_q, [=] {
		++_query.page;
		rebuildRows();
	}));
	_paging[0]->setAccessibleName(tr::lng_management_previous(tr::now));
	_paging[1]->setAccessibleName(tr::lng_management_next(tr::now));
	_selectPage = Ui::CreateChild<Ui::Checkbox>(_header.data(), QString(), false,
		st::managementTableCheckbox, st::managementTableCheck);
	_selectPage->setAccessibleName(tr::lng_management_select_page(tr::now));
	_selectPage->checkedChanges() | rpl::on_next([=](bool checked) {
		for (const auto &id : _page.ids) {
			if (_selection.contains(id) != checked) {
				_selection.toggle(id);
			}
		}
		rebuildRows();
	}, lifetime());
	_header->paintRequest() | rpl::on_next([=] {
		auto p = Painter(_header);
		p.fillRect(_header->rect(), st::windowBgOver);
		p.setFont(st::managementTableFont);
		p.setPen(st::windowSubTextFg);
		const auto pad = st::managementTablePadding;
		const auto y = (_header->height() - st::managementTableFont->height) / 2;
		const auto wide = _header->width() >= st::accountLibraryWideTable;
		const auto number = pad + st::managementCheckWidth + st::managementGap;
		p.drawTextLeft(number, y, _header->width(), tr::lng_management_number(tr::now));
		p.drawTextLeft(
			number + st::accountLibraryNumberWidth,
			y,
			_header->width(),
			tr::lng_management_account_column(tr::now));
		if (wide) {
			const auto actions = _header->width() - pad - st::accountLibraryActionsWidth;
			const auto proxy = actions - st::accountLibraryProxyWidth;
			const auto tags = proxy - st::accountLibraryTagsWidth;
			p.drawTextLeft(
				tags - st::accountLibraryUsernameWidth,
				y,
				_header->width(),
				tr::lng_proxy_box_username(tr::now));
			p.drawTextLeft(tags, y, _header->width(), tr::lng_management_tags_column(tr::now));
			p.drawTextLeft(proxy, y, _header->width(), tr::lng_management_proxy_column(tr::now));
			p.drawTextLeft(actions, y, _header->width(), tr::lng_management_actions(tr::now));
		}
	}, lifetime());

	const auto searchIcon = Ui::CreateChild<Ui::RpWidget>(_search.data());
	searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
	searchIcon->resize(st::managementSearchHeight, st::managementSearchHeight);
	searchIcon->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(searchIcon);
		st::managementSearchIcon.paintInCenter(p, searchIcon->rect());
	}, searchIcon->lifetime());

	_number->changes() | rpl::on_next([=] {
		_query.number = _number->getLastText().trimmed();
		_query.page = 0;
		rebuildRows();
	}, lifetime());
	_search->changes() | rpl::on_next([=] {
		_query.search = _search->getLastText();
		_query.page = 0;
		rebuildRows();
	}, lifetime());
	Core::App().domain().accountsChanges() | rpl::on_next([=] { scheduleRefresh(); }, lifetime());
	sizeValue() | rpl::on_next([=] { layout(); }, lifetime());
	style::PaletteChanged() | rpl::on_next([=] { layout(); }, lifetime());
	refresh();
}

Ui::RoundButton *AccountManagement::button(
		QString text,
		Fn<void()> callback,
		const style::RoundButton &st) {
	const auto result = Ui::CreateChild<Ui::RoundButton>(this, rpl::single(text), st);
	result->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	result->setClickedCallback(std::move(callback));
	return result;
}

void AccountManagement::scheduleRefresh() {
	if (_scheduled) {
		return;
	}
	_scheduled = true;
	InvokeQueued(this, [=] {
		_scheduled = false;
		refresh();
	});
}

void AccountManagement::refresh() {
	readMetadata();
	_accountsLifetime.destroy();
	_accounts.clear();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto account = entry.account.get();
		account->sessionChanges() | rpl::on_next([=] { scheduleRefresh(); }, _accountsLifetime);
		account->network().changes() | rpl::on_next([=] { scheduleRefresh(); }, _accountsLifetime);
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		_accounts.push_back({ Identity(account), session->user()->name(),
			session->user()->phone(), Status(account),
			entry.index + 1, session->user()->username() });
		session->changes().peerUpdates(
			session->user(),
			Data::PeerUpdate::Flag::Name | Data::PeerUpdate::Flag::Photo
				| Data::PeerUpdate::Flag::Username | Data::PeerUpdate::Flag::PhoneNumber
		) | rpl::on_next([=] {
			scheduleRefresh();
		}, _accountsLifetime);
	}
	ranges::sort(_accounts, std::greater<>(), &Main::ManagedAccount::number);
	for (const auto &id : _selection.freeze()) {
		if (!FindAccount(id)) {
			_selection.toggle(id);
		}
	}
	rebuildRows();
}

void AccountManagement::rebuildRows() {
	_page = Main::QueryAccounts(_accounts, _metadata, _query);
	_query.page = _page.page;
	_tagFilter->setText(rpl::single(_query.tag.value_or(_query.group
		? _metadata.groups().value(*_query.group, tr::lng_management_ungrouped(tr::now))
		: tr::lng_management_filter_tags(tr::now))));
	_statusFilter->setText(rpl::single(_query.status ? StatusText(*_query.status)
		: tr::lng_management_filter_status(tr::now)));
	_rows = object_ptr<Ui::RpWidget>(this);
	const auto rows = _rows.data();
	const auto empty = _page.ids.isEmpty();
	rows->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = Painter(rows);
		p.fillRect(clip, st::windowBg);
		if (empty) {
			p.setFont(st::managementTableFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(st::managementPadding, st::managementPadding, rows->width(),
				st::managementTableFont->elided(tr::lng_management_empty(tr::now), rows->width() - 2 * st::managementPadding));
		}
	}, rows->lifetime());
	for (auto i = 0; i != _page.ids.size(); ++i) {
		const auto id = _page.ids[i];
		if (const auto account = FindAccount(id)) {
			const auto entry = ranges::find(_accounts, id, &Main::ManagedAccount::id);
			const auto row = Ui::CreateChild<AccountRow>(
				_rows.data(),
				account,
				_metadata,
				entry->number,
				_selection.contains(id),
				[=] { _selection.toggle(id); layout(); },
				[=] { openAccount(id); },
				[=] { bindMenu({ id }); },
				[=] { rowMenu(id); });
			row->move(0, i * st::accountLibraryRowHeight);
			_rows->widthValue() | rpl::on_next([=](int width) {
				row->resize(width, st::accountLibraryRowHeight);
			}, row->lifetime());
		}
	}
	_rows->resize(width(), std::max(1, int(_page.ids.size())) * st::accountLibraryRowHeight);
	_scroll->setOwnedWidget(std::move(_rows));
	layout();
}

void AccountManagement::layout() {
	const auto pad = st::managementPadding;
	const auto gap = st::managementGap;
	const auto available = std::max(1, width() - 2 * pad);
	const auto compact = available < st::accountLibraryWideTable;
	const auto selected = _selection.freeze().size();
	_tagFilter->setFullWidth(std::min(
		st::accountLibraryFilterWidth,
		std::max(1, (available - gap) / 2)));
	_statusFilter->setFullWidth(_tagFilter->width());
	_title->move(pad, gap);
	_subtitle->setText(selected
		? tr::lng_management_selected_count(tr::now, lt_amount, QString::number(selected))
		: tr::lng_management_subtitle(tr::now, lt_amount, QString::number(_accounts.size())));
	_subtitle->setTextColorOverride(selected ? st::windowActiveTextFg->c : st::windowSubTextFg->c);
	_subtitle->resizeToWidth(available);
	_subtitle->move(pad, gap + _title->height() + gap / 2);
	auto right = width() - pad;
	for (const auto tool : { _selectionMenu, _clearSelection,
			_bindSelection, _tagsSelection }) {
		const auto menu = (tool == _selectionMenu);
		tool->setVisible(menu || (!compact && selected));
		tool->setDisabled(!menu && !selected);
		if (!tool->isHidden()) {
			right -= tool->width();
			tool->move(right, (st::accountLibraryHeaderHeight - tool->height()) / 2);
			right -= gap / 2;
		}
	}
	_moveSelection->hide();
	_moveSelection->setDisabled(!selected);
	_title->resizeToWidth(std::max(1, right - pad));
	auto y = st::accountLibraryHeaderHeight + gap;
	right = width() - pad;
	for (const auto tool : { _add, _import }) {
		tool->setFullWidth(std::min(
			available,
			st::managementButton.style.font->width(tool->accessibilityName()) + 2 * gap));
		if (right - tool->width() < pad) {
			y += st::managementControlHeight + gap;
			right = width() - pad;
		}
		right -= tool->width();
		tool->move(right, y);
		right -= gap;
	}
	if (compact) {
		y += st::managementControlHeight + gap;
		_statusFilter->move(pad, y);
		_tagFilter->move(pad + _statusFilter->width() + gap, y);
		y += st::managementControlHeight + gap;
		right = width() - pad;
	} else {
		for (const auto tool : { _tagFilter, _statusFilter }) {
			right -= tool->width();
			tool->move(right, y);
			right -= gap;
		}
	}
	_number->resizeToWidth(st::accountLibraryNumberSearchWidth);
	_number->move(pad, y);
	_search->resizeToWidth(std::max(1, right - pad - _number->width() - gap));
	_search->move(pad + _number->width() + gap, y);
	y += st::managementControlHeight + gap;
	_header->setGeometry(pad, y, available, st::accountLibraryTableHeaderHeight);
	_selectPage->move(
		st::managementTablePadding,
		(st::accountLibraryTableHeaderHeight - _selectPage->height()) / 2);
	_selectPage->setDisabled(_page.ids.isEmpty());
	_selectPage->setChecked(!_page.ids.empty() && ranges::all_of(_page.ids, [&](const QString &id) {
		return _selection.contains(id);
	}), Ui::Checkbox::NotifyAboutChange::DontNotify);
	y += st::accountLibraryTableHeaderHeight;
	const auto content = _scroll->widget();
	const auto contentHeight = content ? content->height() : 0;
	const auto tableHeight = std::min(
		contentHeight,
		std::max(0, height() - y - pad - gap - st::managementControlHeight));
	_scroll->setGeometry(pad, y, available, tableHeight);
	if (content) {
		content->resize(available, contentHeight);
	}
	y += tableHeight + gap;
	_summary->setText(tr::lng_management_range(
		tr::now,
		lt_current,
		(_page.total ? QString::number(_page.page * _query.pageSize + 1) : u"0"_q)
			+ u"–"_q + QString::number(_page.page * _query.pageSize + _page.ids.size()),
		lt_total,
		QString::number(_page.total),
		lt_amount,
		QString::number(_query.pageSize)));
	_summary->resizeToWidth(std::max(1, available / 2));
	_summary->move(pad, y + (st::managementControlHeight - _summary->height()) / 2);
	_paging[1]->move(width() - pad - _paging[1]->width(), y);
	_pageNumber->setText(QString::number(_page.page + 1) + u" / "_q + QString::number(_page.pages));
	_pageNumber->resizeToWidth(_pageNumber->textMaxWidth());
	_pageNumber->move(_paging[1]->x() - gap - _pageNumber->width(),
		y + (_paging[1]->height() - _pageNumber->height()) / 2);
	_paging[0]->move(_pageNumber->x() - gap - _paging[0]->width(), y);
	if (_summary->x() + _summary->width() + gap > _paging[0]->x()) {
		_summary->hide();
	} else {
		_summary->show();
	}
	_selectAll->move(pad + _summary->textMaxWidth() + gap, y);
	_selectAll->setVisible(!compact
		&& _selectAll->x() + _selectAll->width() + gap < _paging[0]->x());
	_selectAll->setDisabled(_page.matching.isEmpty());
	_paging[0]->setDisabled(_page.page == 0);
	_paging[1]->setDisabled(_page.page + 1 == _page.pages);
	update();
}

bool AccountManagement::readMetadata() {
	_writable = _metadata.load(Core::App().settings().readPref<QByteArray>(kMetadataPref));
	return _writable;
}

void AccountManagement::save() {
	Core::App().settings().writePref<QByteArray>(kMetadataPref, _metadata.serialize());
	Local::writeSettings();
	scheduleRefresh();
}

void AccountManagement::editText(QString title, QString value, Fn<bool(QString)> save) {
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, rpl::single(title), value));
		box->addButton(tr::lng_settings_save(), [=] {
			if (save(field->getLastText())) {
				box->closeBox();
			} else {
				field->showError();
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->setFocusCallback([=] { field->setFocus(); });
	}));
}

void AccountManagement::editGroup(QString id) {
	if (!_writable) {
		_controller->showToast(tr::lng_management_unreadable(tr::now));
		return;
	}
	editText(tr::lng_management_group_name(tr::now), _metadata.groups().value(id),
		crl::guard(this, [=](QString text) {
			if (!readMetadata()) return false;
			const auto good = id.isEmpty() ? !_metadata.addGroup(text).isEmpty()
				: _metadata.renameGroup(id, text);
			if (good) save();
			return good;
		}));
}

void AccountManagement::groupMenu(bool assign) {
	if (!_writable) {
		_controller->showToast(tr::lng_management_unreadable(tr::now));
		return;
	}
	const auto targets = FreezeTargets(_selection.freeze());
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	if (!assign) {
		_menu->addAction(tr::lng_management_create_group(tr::now), [=] { editGroup(); });
	} else {
		_menu->addAction(tr::lng_management_ungrouped(tr::now), [=] {
			moveToGroup({}, targets);
		});
	}
	for (auto i = _metadata.groups().begin(); i != _metadata.groups().end(); ++i) {
		const auto group = i.key();
		if (assign) {
			_menu->addAction(i.value(), [=] {
				moveToGroup(group, targets);
			});
		} else {
			_menu->addAction(
				tr::lng_management_rename(tr::now) + u": "_q + i.value(),
				[=] { editGroup(group); });
			_menu->addAction(tr::lng_management_delete(tr::now) + u": "_q + i.value(), [=] {
				_controller->show(Ui::MakeConfirmBox({
					.text = tr::lng_management_delete_group_confirm(),
					.confirmed = crl::guard(this, [=](Fn<void()> close) {
						if (readMetadata()) {
							_metadata.removeGroup(group);
							if (_query.group == group) _query.group = QString();
							save();
						}
						close();
					}),
				}));
			});
		}
	}
	_menu->popup(QCursor::pos());
}

void AccountManagement::moveToGroup(
		QString group,
		const std::vector<AccountTarget> &targets) {
	const auto writable = readMetadata();
	const auto exists = group.isEmpty() || _metadata.groups().contains(group);
	if (!writable || !exists) {
		_controller->showToast(tr::lng_management_move_failed(tr::now));
		return;
	}
	for (const auto &target : targets) {
		if (FindAccount(target.id)) {
			_metadata.assign(target.id, group);
		}
	}
	save();
	_controller->showToast(tr::lng_management_moved(tr::now));
}

void AccountManagement::openAccount(QString id) {
	if (const auto account = FindAccount(id)) {
		_controller->widget()->closeAccountManagement();
		Core::App().domain().maybeActivate(account);
	}
}

void AccountManagement::logoutAccount(QString id) {
	const auto account = FindAccount(id);
	if (!account) {
		return;
	}
	_controller->show(Ui::MakeConfirmBox({
		.text = Window::LogoutConfirmationText(account),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto target = FindAccount(id)) {
				Core::App().logoutWithChecks(target);
			}
		},
		.confirmText = tr::lng_settings_logout(),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

void AccountManagement::rowMenu(QString id) {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_management_open(tr::now), [=] { openAccount(id); });
	_menu->addAction(tr::lng_management_bind(tr::now), [=] {
		Ui::PostponeCall(this, [=] { bindMenu({ id }); });
	});
	_menu->addAction(tr::lng_management_tags_column(tr::now), [=] { editTags({ id }); });
	_menu->addAction(tr::lng_settings_logout(tr::now), [=] { logoutAccount(id); });
	_menu->popup(QCursor::pos());
}

void AccountManagement::editTags(QStringList ids) {
	if (ids.isEmpty()) {
		return;
	}
	if (!_writable) {
		_controller->showToast(tr::lng_management_unreadable(tr::now));
		return;
	}
	editText(
		ids.size() == 1 ? tr::lng_management_tags(tr::now)
			: tr::lng_management_add_tags(tr::now),
		ids.size() == 1 ? _metadata.tags(ids.front()).join(u", "_q) : QString(),
		crl::guard(this, [=](QString text) {
			if (!readMetadata()) {
				return false;
			}
			for (const auto &id : ids) {
				if (FindAccount(id)) {
					_metadata.setTags(
						id,
						ids.size() == 1 ? text.split(',')
							: _metadata.tags(id) + text.split(','));
				}
			}
			save();
			return true;
		}));
}

void AccountManagement::bindMenu(QStringList ids) {
	if (ids.isEmpty()) return;
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	for (const auto &entry : Core::App().settings().proxyLibrary().entries()) {
		if (!Core::ProxyLibrary::Supported(entry.proxy)) continue;
		const auto id = entry.id;
		_menu->addAction(entry.proxy.host + u":"_q + QString::number(entry.proxy.port),
			[=] { confirmBinding(id, ids); });
	}
	_menu->addAction(tr::lng_management_proxies(tr::now), [=] {
		_controller->widget()->showAccountManagement(true);
	});
	_menu->popup(QCursor::pos());
}

void AccountManagement::confirmBinding(QString proxy, QStringList ids) {
	const auto targets = FreezeTargets(ids);
	const auto entry = Core::App().settings().proxyLibrary().find(proxy);
	if (!entry) return;
	const auto revision = entry->revision;
	auto names = QStringList();
	auto existing = 0;
	auto added = 0;
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account->proxyBindingId() == proxy) ++existing;
	}
	for (const auto &target : targets) {
		names.push_back(target.name);
		if (const auto account = target.account.get()) {
			if (account->proxyBindingId() != proxy) ++added;
		}
	}
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_management_bind());
		const auto text = tr::lng_management_shared(
			tr::now,
			lt_amount,
			QString::number(existing),
			lt_selected,
			QString::number(targets.size()),
			lt_added,
			QString::number(added)) + u"\n\n"_q + names.join('\n');
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, text, st::defaultFlatLabel));
		box->addButton(tr::lng_management_bind(), crl::guard(this, [=] {
			const auto current = Core::App().settings().proxyLibrary().find(proxy);
			const auto unchanged = current && current->revision == revision;
			auto operations = std::vector<uint64>();
			for (const auto &target : targets) {
				const auto account = target.account.get();
				const auto valid = unchanged && account && Identity(account) == target.id;
				const auto accepted = valid && account->bindProxy(proxy);
				operations.push_back(accepted
					? account->proxyBindingOperation() : 0);
			}
			box->closeBox();
			BindingResults(_controller, targets, std::move(operations));
			scheduleRefresh();
		}));
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void AccountManagement::showTagFilterMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_management_filter_tags(tr::now), [=] {
		_query.tag.reset();
		_query.group.reset();
		_query.page = 0;
		rebuildRows();
	});
	auto tags = QStringList();
	for (const auto &account : _accounts) {
		tags.append(_metadata.tags(account.id));
	}
	tags.removeDuplicates();
	tags.sort(Qt::CaseInsensitive);
	for (const auto &tag : tags) {
		_menu->addAction(tag, [=] {
			_query.tag = tag;
			_query.group.reset();
			_query.page = 0;
			rebuildRows();
		});
	}
	_menu->addSeparator();
	auto groups = _metadata.groups();
	groups.insert(QString(), tr::lng_management_ungrouped(tr::now));
	for (auto i = groups.begin(); i != groups.end(); ++i) {
		const auto group = i.key();
		_menu->addAction(tr::lng_management_group_column(tr::now) + u": "_q + i.value(), [=] {
			_query.group = group;
			_query.tag.reset();
			_query.page = 0;
			rebuildRows();
		});
	}
	_menu->popup(QCursor::pos());
}

void AccountManagement::showStatusFilterMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_management_all(tr::now), [=] {
		_query.status.reset();
		_query.page = 0;
		rebuildRows();
	});
	for (const auto state : {
		Main::AccountStatus::Ready,
		Main::AccountStatus::Unbound,
		Main::AccountStatus::Checking,
		Main::AccountStatus::Failed,
		Main::AccountStatus::Direct,
	}) {
		_menu->addAction(StatusText(state), [=] {
			_query.status = state;
			_query.page = 0;
			rebuildRows();
		});
	}
	_menu->popup(QCursor::pos());
}

} // namespace

object_ptr<Ui::RpWidget> CreateAccountManagement(
		QWidget *parent,
		not_null<Window::Controller*> controller) {
	return object_ptr<AccountManagement>(parent, controller);
}

} // namespace Settings
