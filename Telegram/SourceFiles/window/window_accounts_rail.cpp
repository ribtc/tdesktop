/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_accounts_rail.h"

#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/network_context.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "ui/unread_badge_paint.h"
#include "ui/userpic_view.h"
#include "window/window_controller.h"

#include "styles/style_window.h"

namespace Window {
namespace {

class AccountList final : public Ui::RpWidget {
public:
	AccountList(
		QWidget *parent,
		not_null<Controller*> controller,
		not_null<Ui::ScrollArea*> scroll);
	[[nodiscard]] rpl::producer<int> countValue() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	struct Row {
		base::weak_ptr<Main::Account> account;
		Ui::PeerUserpicView userpic;
	};

	void refresh();
	void scheduleRefresh();
	void select(int index);
	void activate(int index);

	const not_null<Controller*> _controller;
	const not_null<Ui::ScrollArea*> _scroll;
	std::vector<Row> _rows;
	rpl::variable<int> _count = 0;
	int _selected = -1;
	int _pressed = -1;
	bool _refreshScheduled = false;
	rpl::lifetime _rowsLifetime;

};

AccountList::AccountList(
	QWidget *parent,
	not_null<Controller*> controller,
	not_null<Ui::ScrollArea*> scroll)
: RpWidget(parent)
, _controller(controller)
, _scroll(scroll) {
	setFocusPolicy(Qt::StrongFocus);
	setCursor(style::cur_pointer);
	setMouseTracking(true);
	auto &domain = Core::App().domain();
	domain.accountsChanges() | rpl::on_next([=] {
		scheduleRefresh();
	}, lifetime());
	domain.activeValue() | rpl::on_next([=](Main::Account *account) {
		for (auto i = 0; i != int(_rows.size()); ++i) {
			if (_rows[i].account.get() == account) {
				select(i);
				break;
			}
		}
		update();
	}, lifetime());
	refresh();
	_scroll->heightValue() | rpl::on_next([=] {
		select(_selected);
	}, lifetime());
}

rpl::producer<int> AccountList::countValue() const {
	return _count.value();
}

void AccountList::scheduleRefresh() {
	if (_refreshScheduled) {
		return;
	}
	_refreshScheduled = true;
	Ui::PostponeCall(this, [=] {
		_refreshScheduled = false;
		refresh();
	});
}

void AccountList::refresh() {
	_rowsLifetime.destroy();
	_rows.clear();
	auto &domain = Core::App().domain();
	for (const auto account : domain.orderedAccounts()) {
		account->sessionChanges() | rpl::on_next([=] {
			scheduleRefresh();
		}, _rowsLifetime);
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		_rows.push_back({ .account = base::make_weak(account.get()) });
		session->downloaderTaskFinished() | rpl::on_next([=] {
			update();
		}, _rowsLifetime);
		session->data().unreadBadgeChanges() | rpl::on_next([=] {
			update();
		}, _rowsLifetime);
		session->changes().peerUpdates(
			session->user(),
			Data::PeerUpdate::Flag::Name | Data::PeerUpdate::Flag::Photo
		) | rpl::on_next([=] {
			update();
		}, _rowsLifetime);
		account->network().changes() | rpl::on_next([=] {
			update();
		}, _rowsLifetime);
	}
	_count = int(_rows.size());
	resize(st::windowAccountsWidth, _count.current() * st::windowAccountsRow);
	_selected = -1;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].account.get() == &domain.active()) {
			select(i);
			break;
		}
	}
	update();
}

void AccountList::select(int index) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	_selected = index;
	const auto top = index * st::windowAccountsRow;
	_scroll->scrollToY(top, top + st::windowAccountsRow);
	const auto account = _rows[index].account.get();
	if (const auto session = account ? account->maybeSession() : nullptr) {
		setAccessibleName(session->user()->name());
	}
	update();
}

void AccountList::activate(int index) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	if (const auto account = _rows[index].account.get()) {
		const auto controller = _controller;
		controller->widget()->closeAccountManagement();
		Core::App().setActivePrimaryWindow(controller);
		Core::App().domain().maybeActivate(account);
	}
}

void AccountList::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	p.fillRect(e->rect(), st::sideBarBg);
	const auto rowHeight = st::windowAccountsRow;
	const auto from = std::max(0, e->rect().top() / rowHeight);
	const auto till = std::min(
		int(_rows.size()),
		e->rect().bottom() / rowHeight + 1);
	for (auto i = from; i < till; ++i) {
		auto &row = _rows[i];
		const auto account = row.account.get();
		const auto session = account ? account->maybeSession() : nullptr;
		if (!session) {
			continue;
		}
		const auto top = i * rowHeight;
		const auto size = st::windowAccountsUserpic;
		const auto left = (width() - size) / 2;
		const auto photoTop = top + st::windowAccountsPhotoTop;
		const auto active = account == &_controller->account();
		if (hasFocus() && i == _selected) {
			p.fillRect(0, top, width(), rowHeight, st::sideBarBgActive);
		}
		session->user()->paintUserpicLeft(
			p,
			row.userpic,
			left,
			photoTop,
			width(),
			size);
		if (active) {
			const auto gap = st::windowAccountsRingGap;
			auto hq = PainterHighQualityEnabler(p);
			auto pen = st::windowBgActive->p;
			pen.setWidth(st::windowAccountsRingWidth);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(QRect(left, photoTop, size, size).marginsAdded(
				QMargins(gap, gap, gap, gap)));
		}
		const auto unread = session->data().unreadWithMentionsBadge();
		if (unread) {
			auto badge = Ui::UnreadBadgeStyle();
			badge.muted = session->data().unreadWithMentionsBadgeMuted();
			badge.sizeId = Ui::UnreadBadgeSize::MainMenu;
			Ui::PaintUnreadBadge(
				p,
				(unread > 99) ? u"99+"_q : QString::number(unread),
				width() - st::windowAccountsBadgeSkip,
				top,
				badge);
		} else if (!account->network().snapshot().allowed()) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::sideBarBadgeBgMuted);
			const auto dot = st::windowAccountsStatusSize;
			p.drawEllipse(left + size - dot, photoTop + size - dot, dot, dot);
		}
		p.setFont(st::windowAccountsNameFont);
		p.setPen(active ? st::windowBgActive : st::sideBarTextFg);
		const auto name = st::windowAccountsNameFont->elided(
			session->user()->name(),
			width() - 2 * st::windowAccountsBadgeSkip);
		p.drawText(
			QRect(
				0,
				photoTop + size,
				width(),
				rowHeight - st::windowAccountsPhotoTop - size),
			Qt::AlignCenter,
			name);
	}
}

void AccountList::mouseMoveEvent(QMouseEvent *e) {
	const auto index = int(e->position().y()) / st::windowAccountsRow;
	const auto account = (index >= 0 && index < int(_rows.size()))
		? _rows[index].account.get()
		: nullptr;
	const auto session = account ? account->maybeSession() : nullptr;
	setToolTip(session ? session->user()->name() : QString());
}

void AccountList::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton)
		? int(e->position().y()) / st::windowAccountsRow
		: -1;
	select(_pressed);
}

void AccountList::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = _pressed;
	_pressed = -1;
	if (e->button() == Qt::LeftButton
		&& rect().contains(e->position().toPoint())
		&& int(e->position().y()) / st::windowAccountsRow == pressed) {
		activate(pressed);
	}
}

void AccountList::keyPressEvent(QKeyEvent *e) {
	switch (e->key()) {
	case Qt::Key_Up: select(std::max(0, _selected - 1)); break;
	case Qt::Key_Down: select(std::min(int(_rows.size()) - 1, _selected + 1)); break;
	case Qt::Key_Home: select(0); break;
	case Qt::Key_End: select(int(_rows.size()) - 1); break;
	case Qt::Key_Return:
	case Qt::Key_Enter:
	case Qt::Key_Space: activate(_selected); break;
	default: RpWidget::keyPressEvent(e); return;
	}
	e->accept();
}

} // namespace

AccountsRail::AccountsRail(QWidget *parent, not_null<Controller*> controller)
: RpWidget(parent) {
	setVisualTabOrder(true);
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_accounts_rail_title(),
		st::windowAccountsTitle);
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(this);
	const auto list = scroll->setOwnedWidget(
		object_ptr<AccountList>(scroll, controller, scroll));
	const auto all = Ui::CreateChild<Ui::IconButton>(
		this,
		st::windowAccountsAll);
	const auto add = Ui::CreateChild<Ui::IconButton>(
		this,
		st::windowAccountsAdd);
	const auto allLabel = Ui::CreateChild<Ui::FlatLabel>(
		all,
		tr::lng_accounts_all_short(),
		st::windowAccountsTitle);
	const auto count = Ui::CreateChild<Ui::FlatLabel>(
		all,
		list->countValue() | rpl::map([](int value) {
			return QString::number(value);
		}),
		st::windowAccountsCount);
	allLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	count->setAttribute(Qt::WA_TransparentForMouseEvents);
	allLabel->resizeToWidth(st::windowAccountsWidth);
	count->resizeToWidth(st::windowAccountsWidth);
	allLabel->move(0, st::windowAccountsAllTextTop);
	count->move(0, st::windowAccountsCountTop);
	all->setClickedCallback([=] { controller->widget()->showAccounts(); });
	add->setClickedCallback([=] {
		controller->preventOrInvoke(crl::guard(this, [=] {
			Core::App().setActivePrimaryWindow(controller);
			Core::App().domain().addActivated(MTP::Environment::Production);
		}));
	});
	tr::lng_accounts_all() | rpl::on_next([=](const QString &text) {
		all->setAccessibleName(text);
		all->setToolTip(text);
	}, lifetime());
	tr::lng_menu_add_account() | rpl::on_next([=](const QString &text) {
		add->setAccessibleName(text);
		add->setToolTip(text);
	}, lifetime());
	sizeValue() | rpl::on_next([=](QSize size) {
		title->resizeToWidth(size.width());
		title->move(0, st::windowAccountsTitleTop);
		const auto footer = all->height() + add->height();
		const auto top = st::windowAccountsHeader;
		scroll->setGeometry(
			0,
			top,
			size.width(),
			std::max(0, size.height() - top - footer));
		all->move(0, size.height() - footer);
		add->move(0, size.height() - add->height());
	}, lifetime());
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = Painter(this);
		p.fillRect(clip, st::sideBarBg);
	}, lifetime());
}

} // namespace Window
