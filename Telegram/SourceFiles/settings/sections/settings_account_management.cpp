#include "settings/sections/settings_account_management.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/proxy_library.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
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
	default: return Main::AccountStatus::Unbound;
	}
}

QString StatusText(Main::AccountStatus status) {
	switch (status) {
	case Main::AccountStatus::Ready: return tr::lng_management_ready(tr::now);
	case Main::AccountStatus::Checking: return tr::lng_management_checking(tr::now);
	case Main::AccountStatus::Failed: return tr::lng_management_failed(tr::now);
	case Main::AccountStatus::Unbound: return tr::lng_management_unbound(tr::now);
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

void BindingResults(
		not_null<Window::Controller*> controller,
		std::vector<AccountTarget> targets,
		std::vector<uint64> operations) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_management_results());
		struct State {
			std::vector<AccountTarget> targets;
			std::vector<uint64> operations;
		};
		const auto state = box->lifetime().make_state<State>(targets, operations);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box, QString(), st::defaultFlatLabel));
		const auto update = [=] {
			auto results = QStringList();
			for (auto i = 0; i != int(state->targets.size()); ++i) {
				const auto &target = state->targets[i];
				const auto account = target.account.get();
				const auto valid = state->operations[i]
					&& account
					&& Identity(account) == target.id;
				const auto changed = valid
					&& account->proxyBindingOperation() != state->operations[i];
				const auto status = changed ? tr::lng_management_replaced(tr::now)
					: valid ? StatusText(Status(account))
					: tr::lng_management_rejected(tr::now);
				results.push_back(target.name + u": "_q + status);
			}
			label->setText(results.join('\n'));
		};
		for (const auto &target : targets) {
			if (const auto account = target.account.get()) {
				account->network().changes() | rpl::on_next(update, box->lifetime());
				account->sessionChanges() | rpl::on_next(update, box->lifetime());
			}
		}
		update();
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

class AccountRow final : public Ui::RpWidget {
public:
	AccountRow(
		QWidget *parent,
		not_null<Main::Account*> account,
		const Main::AccountMetadata &metadata,
		bool selected,
		Fn<void()> toggle,
		Fn<void()> actions);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	base::weak_ptr<Main::Account> _account;
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
	void showGroupFilterMenu();
	void showStatusFilterMenu();
	void moveToGroup(QString group, const std::vector<AccountTarget> &targets);
	bool readMetadata();
	void editGroup(QString id = {});
	void rowMenu(QString id);
	void editTags(QString id);
	void bindMenu();
	void confirmBinding(QString proxy, QStringList ids);
	void showResults(QStringList results);
	void editText(QString title, QString value, Fn<bool(QString)> save);
	Ui::LinkButton *button(QString text, Fn<void()> callback);

	const not_null<Window::Controller*> _controller;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::InputField> _search;
	object_ptr<Ui::ScrollArea> _scroll;
	object_ptr<Ui::RpWidget> _rows = { nullptr };
	object_ptr<Ui::FlatLabel> _summary;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Ui::LinkButton *_groupFilter = nullptr;
	Ui::LinkButton *_statusFilter = nullptr;
	Ui::LinkButton *_moveSelection = nullptr;
	Ui::LinkButton *_bindSelection = nullptr;
	std::vector<Ui::LinkButton*> _tools;
	std::vector<Ui::LinkButton*> _selectionTools;
	std::vector<Ui::LinkButton*> _paging;
	std::vector<Ui::LinkButton*> _compactTools;
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
	bool selected,
	Fn<void()> toggle,
	Fn<void()> actions)
: RpWidget(parent)
, _account(base::make_weak(account.get()))
, _group(metadata.groups().value(metadata.group(Identity(account)),
	tr::lng_management_ungrouped(tr::now)))
, _tags(metadata.tags(Identity(account)).join(u", "_q))
, _selected(selected) {
	resize(width(), st::managementRowHeight);
	const auto check = Ui::CreateChild<Ui::Checkbox>(this, QString(), selected);
	check->setAccessibleName(account->session().user()->name());
	check->checkedChanges() | rpl::on_next([=](bool checked) {
		_selected = checked;
		toggle();
		update();
	}, lifetime());
	const auto menu = Ui::CreateChild<Ui::LinkButton>(this, u"⋯"_q);
	menu->setAccessibleName(tr::lng_management_actions(tr::now));
	menu->setClickedCallback(std::move(actions));
	sizeValue() | rpl::on_next([=](QSize size) {
		check->move(st::managementGap, (size.height() - check->height()) / 2);
		menu->move(size.width() - st::managementPadding, (size.height() - menu->height()) / 2);
	}, lifetime());
}

void AccountRow::paintEvent(QPaintEvent *e) {
	const auto account = _account.get();
	const auto session = account ? account->maybeSession() : nullptr;
	if (!session) {
		return;
	}
	auto p = Painter(this);
	p.fillRect(e->rect(), _selected ? st::sideBarBgActive : st::windowBg);
	const auto pad = st::managementPadding;
	const auto size = st::managementAvatarSize;
	const auto left = pad * 2;
	session->user()->paintUserpicLeft(p, _userpic, left, (height() - size) / 2, width(), size);
	const auto textLeft = left + size + st::managementGap;
	const auto wide = width() >= st::managementWideTable;
	const auto column = wide ? st::managementColumnWidth : 0;
	const auto textWidth = std::max(0, width() - textLeft - 2 * pad - 2 * column);
	p.setFont(st::normalFont);
	p.setPen(st::windowFg);
	p.drawText(textLeft, pad, st::normalFont->elided(session->user()->name(), textWidth));
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	const auto details = session->user()->phone() + u"  "_q + _tags;
	p.drawText(textLeft, pad + st::normalFont->height, st::normalFont->elided(details, textWidth));
	const auto state = StatusText(Status(account));
	p.drawText(
		textLeft,
		pad + 2 * st::normalFont->height,
		st::normalFont->elided(state, textWidth));
	if (wide) {
		p.drawText(width() - 2 * column - pad, height() / 2,
			st::normalFont->elided(_group, column - st::managementGap));
		const auto proxy = Core::App().settings().proxyLibrary().find(account->proxyBindingId());
		const auto label = proxy ? proxy->proxy.host : tr::lng_management_unbound(tr::now);
		p.drawText(width() - column - pad, height() / 2,
			st::normalFont->elided(label, column - st::managementGap));
	}
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
}

AccountManagement::AccountManagement(
	QWidget *parent,
	not_null<Window::Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _title(this, tr::lng_management_accounts(), st::defaultFlatLabel)
, _search(this, st::defaultInputField, tr::lng_management_search())
, _scroll(this, st::defaultScrollArea)
, _summary(this, QString(), st::defaultFlatLabel) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusProxy(_search);
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(this);
		p.fillRect(clip, st::windowBg);
	}, lifetime());
	_tools.push_back(button(tr::lng_management_add(tr::now), [=] {
		const auto environment = _controller->account().mtp().isTestMode()
			? MTP::Environment::Test : MTP::Environment::Production;
		Core::App().domain().addActivated(environment);
	}));
	_tools.push_back(button(tr::lng_management_groups(tr::now), [=] { groupMenu(false); }));
	_groupFilter = button(tr::lng_management_filter_group(tr::now), [=] {
		showGroupFilterMenu();
	});
	_statusFilter = button(tr::lng_management_filter_status(tr::now), [=] {
		showStatusFilterMenu();
	});
	_tools.push_back(_groupFilter);
	_tools.push_back(_statusFilter);
	_selectionTools.push_back(button(tr::lng_management_select_page(tr::now), [=] {
		_selection.select(_page.ids); rebuildRows();
	}));
	_selectionTools.push_back(button(tr::lng_management_select_all(tr::now), [=] {
		_selection.select(_page.matching); rebuildRows();
	}));
	_selectionTools.push_back(button(tr::lng_management_clear(tr::now), [=] {
		_selection.clear(); rebuildRows();
	}));
	_moveSelection = button(tr::lng_management_move(tr::now), [=] { groupMenu(true); });
	_bindSelection = button(tr::lng_management_bind(tr::now), [=] { bindMenu(); });
	_selectionTools.push_back(_moveSelection);
	_selectionTools.push_back(_bindSelection);
	_paging.push_back(button(tr::lng_management_previous(tr::now), [=] {
		--_query.page; rebuildRows();
	}));
	_paging.push_back(button(tr::lng_management_next(tr::now), [=] {
		++_query.page; rebuildRows();
	}));
	for (const auto selection : { false, true }) {
		_compactTools.push_back(button(selection
			? tr::lng_management_selection(tr::now)
			: tr::lng_management_actions(tr::now), [=] {
			_menu = base::make_unique_q<Ui::PopupMenu>(this);
			const auto &buttons = selection ? _selectionTools : _tools;
			for (const auto button : buttons) {
				if (button->isDisabled()) continue;
				_menu->addAction(button->accessibilityName(), [=] {
					Ui::PostponeCall(this, [=] {
						button->clicked(Qt::NoModifier, Qt::LeftButton);
					});
				});
			}
			_menu->popup(QCursor::pos());
		}));
	}
	_search->changes() | rpl::on_next([=] {
		_query.search = _search->getLastText();
		_query.page = 0;
		rebuildRows();
	}, lifetime());
	Core::App().domain().accountsChanges() | rpl::on_next([=] { scheduleRefresh(); }, lifetime());
	sizeValue() | rpl::on_next([=] { layout(); }, lifetime());
	refresh();
}

Ui::LinkButton *AccountManagement::button(QString text, Fn<void()> callback) {
	const auto result = Ui::CreateChild<Ui::LinkButton>(this, text);
	result->setClickedCallback(std::move(callback));
	return result;
}

void AccountManagement::scheduleRefresh() {
	if (_scheduled) {
		return;
	}
	_scheduled = true;
	Ui::PostponeCall(this, [=] { _scheduled = false; refresh(); });
}

void AccountManagement::refresh() {
	readMetadata();
	_accountsLifetime.destroy();
	_accounts.clear();
	for (const auto account : Core::App().domain().orderedAccounts()) {
		account->sessionChanges() | rpl::on_next([=] { scheduleRefresh(); }, _accountsLifetime);
		account->network().changes() | rpl::on_next([=] { scheduleRefresh(); }, _accountsLifetime);
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		_accounts.push_back({ Identity(account), session->user()->name(),
			session->user()->phone(), Status(account) });
		session->changes().peerUpdates(
			session->user(),
			Data::PeerUpdate::Flag::Name | Data::PeerUpdate::Flag::Photo
		) | rpl::on_next([=] {
			scheduleRefresh();
		}, _accountsLifetime);
	}
	rebuildRows();
}

void AccountManagement::rebuildRows() {
	_page = Main::QueryAccounts(_accounts, _metadata, _query);
	_query.page = _page.page;
	_groupFilter->setText(_query.group
		? _metadata.groups().value(*_query.group, tr::lng_management_ungrouped(tr::now))
		: tr::lng_management_filter_group(tr::now));
	_statusFilter->setText(_query.status ? StatusText(*_query.status)
		: tr::lng_management_filter_status(tr::now));
	_rows = object_ptr<Ui::RpWidget>(this);
	const auto rows = _rows.data();
	const auto empty = _page.ids.isEmpty();
	rows->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = Painter(rows);
		p.fillRect(clip, st::windowBg);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto pad = st::managementPadding;
		p.drawText(pad, pad, tr::lng_management_account_column(tr::now));
		if (rows->width() >= st::managementWideTable) {
			p.drawText(rows->width() - 2 * st::managementColumnWidth - pad, pad,
				tr::lng_management_group_column(tr::now));
			p.drawText(rows->width() - st::managementColumnWidth - pad, pad,
				tr::lng_management_proxy_column(tr::now));
		}
		if (empty) {
			p.drawText(pad, st::managementControlHeight + pad,
				st::normalFont->elided(tr::lng_management_empty(tr::now), rows->width() - 2 * pad));
		}
	}, rows->lifetime());
	for (auto i = 0; i != _page.ids.size(); ++i) {
		const auto id = _page.ids[i];
		if (const auto account = FindAccount(id)) {
			const auto row = Ui::CreateChild<AccountRow>(_rows.data(), account, _metadata,
				_selection.contains(id), [=] { _selection.toggle(id); layout(); },
				[=] { rowMenu(id); });
			row->move(0, st::managementControlHeight + i * st::managementRowHeight);
			_rows->widthValue() | rpl::on_next([=](int width) {
				row->resize(width, st::managementRowHeight);
			}, row->lifetime());
		}
	}
	_rows->resize(width(), st::managementControlHeight
		+ std::max(1, int(_page.ids.size())) * st::managementRowHeight);
	_scroll->setOwnedWidget(std::move(_rows));
	layout();
}

void AccountManagement::layout() {
	const auto pad = st::managementPadding;
	const auto available = std::max(1, width() - 2 * pad);
	auto y = pad;
	_title->move(pad, y);
	_title->resizeToWidth(available);
	y += _title->height() + st::managementGap;
	const auto place = [&](const std::vector<Ui::LinkButton*> &buttons) {
		auto x = pad;
		for (const auto button : buttons) {
			button->resizeToWidth(std::min(available, button->naturalWidth()));
			if (x > pad && x + button->width() > width() - pad) {
				x = pad;
				y += st::managementControlHeight;
			}
			button->move(x, y);
			x += button->width() + st::managementGap;
		}
		y += st::managementControlHeight;
	};
	const auto compact = available < st::managementWideTable;
	for (const auto button : _compactTools) button->setVisible(compact);
	for (const auto button : _tools) button->setVisible(!compact);
	for (const auto button : _selectionTools) button->setVisible(!compact);
	place(compact ? _compactTools : _tools);
	_search->resizeToWidth(available);
	_search->move(pad, y);
	y += _search->height() + st::managementGap;
	if (!compact) place(_selectionTools);
	_summary->setText(tr::lng_management_summary(
		tr::now,
		lt_amount,
		QString::number(_page.total),
		lt_selected,
		QString::number(_selection.freeze().size()),
		lt_current,
		QString::number(_page.page + 1),
		lt_total,
		QString::number(_page.pages)));
	_summary->resizeToWidth(available);
	_summary->move(pad, y);
	y += _summary->height() + st::managementGap;
	_scroll->setGeometry(pad, y, available,
		std::max(0, height() - y - pad - st::managementControlHeight));
	if (const auto content = _scroll->widget()) {
		content->resize(available, content->height());
	}
	y = height() - st::managementControlHeight;
	_paging[0]->setText(compact ? u"‹"_q : tr::lng_management_previous(tr::now));
	_paging[1]->setText(compact ? u"›"_q : tr::lng_management_next(tr::now));
	place(_paging);
	_paging[0]->setDisabled(_page.page == 0);
	_paging[1]->setDisabled(_page.page + 1 == _page.pages);
	const auto emptySelection = _selection.freeze().isEmpty();
	_moveSelection->setDisabled(emptySelection);
	_bindSelection->setDisabled(emptySelection);
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
	auto results = QStringList();
	for (const auto &target : targets) {
		const auto account = target.account.get();
		const auto valid = writable && exists && account && Identity(account) == target.id;
		if (valid) {
			_metadata.assign(target.id, group);
		}
		results.push_back(target.name + u": "_q + (valid
			? tr::lng_management_moved(tr::now) : tr::lng_management_move_failed(tr::now)));
	}
	if (writable) save();
	showResults(std::move(results));
}

void AccountManagement::rowMenu(QString id) {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_management_open(tr::now), [=] {
		if (const auto account = FindAccount(id)) {
			const auto controller = _controller;
			controller->widget()->closeAccountManagement();
			Core::App().domain().maybeActivate(account);
		}
	});
	_menu->addAction(tr::lng_management_tags(tr::now), [=] { editTags(id); });
	_menu->popup(QCursor::pos());
}

void AccountManagement::editTags(QString id) {
	if (!_writable) {
		_controller->showToast(tr::lng_management_unreadable(tr::now));
		return;
	}
	editText(tr::lng_management_tags(tr::now), _metadata.tags(id).join(u", "_q),
		crl::guard(this, [=](QString text) {
			if (!FindAccount(id) || !readMetadata()) return false;
			_metadata.setTags(id, text.split(','));
			save();
			return true;
		}));
}

void AccountManagement::bindMenu() {
	const auto ids = _selection.freeze();
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

void AccountManagement::showGroupFilterMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_management_all(tr::now), [=] {
		_query.group.reset(); rebuildRows();
	});
	_menu->addAction(tr::lng_management_ungrouped(tr::now), [=] {
		_query.group = QString(); rebuildRows();
	});
	for (auto i = _metadata.groups().begin(); i != _metadata.groups().end(); ++i) {
		const auto id = i.key();
		_menu->addAction(i.value(), [=] { _query.group = id; rebuildRows(); });
	}
	_menu->popup(QCursor::pos());
}

void AccountManagement::showStatusFilterMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_management_all(tr::now), [=] {
		_query.status.reset(); rebuildRows();
	});
	for (const auto state : { Main::AccountStatus::Ready, Main::AccountStatus::Unbound,
		Main::AccountStatus::Checking, Main::AccountStatus::Failed }) {
		_menu->addAction(StatusText(state), [=] { _query.status = state; rebuildRows(); });
	}
	_menu->popup(QCursor::pos());
}

void AccountManagement::showResults(QStringList results) {
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_management_results());
		box->addRow(object_ptr<Ui::FlatLabel>(box, results.join('\n'), st::defaultFlatLabel));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace

object_ptr<Ui::RpWidget> CreateAccountManagement(
		QWidget *parent,
		not_null<Window::Controller*> controller) {
	return object_ptr<AccountManagement>(parent, controller);
}

} // namespace Settings
