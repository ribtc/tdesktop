#include "window/window_account_management.h"

#include "lang/lang_keys.h"
#include "core/application.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/sections/settings_account_management.h"
#include "settings/sections/settings_proxy_management.h"
#include "settings/sections/settings_protection.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"
#include "window/window_controller.h"

#include "styles/style_window.h"

namespace Window {
namespace {

enum class ManagementPage { Accounts, Proxies, Protection };

class Management final : public Ui::RpWidget {
public:
	Management(QWidget *parent, not_null<Controller*> controller, bool proxies);

private:
	void showPage(ManagementPage page);
	const not_null<Controller*> _controller;
	object_ptr<Ui::RpWidget> _content = { nullptr };
	ManagementPage _page = ManagementPage::Accounts;

};

Management::Management(
	QWidget *parent,
	not_null<Controller*> controller,
	bool proxies)
: RpWidget(parent)
, _controller(controller) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	const auto buttons = std::array<Ui::AbstractButton*, 4>{
		Ui::CreateChild<Ui::AbstractButton>(this),
		Ui::CreateChild<Ui::AbstractButton>(this),
		Ui::CreateChild<Ui::AbstractButton>(this),
		Ui::CreateChild<Ui::AbstractButton>(this),
	};
	for (auto i = 0; i != int(buttons.size()); ++i) {
		const auto button = buttons[i];
		const auto label = i == 0 ? tr::lng_management_back(tr::now)
			: i == 1 ? tr::lng_management_accounts(tr::now)
			: i == 2 ? tr::lng_management_proxies(tr::now)
			: tr::lng_protection_title(tr::now);
		button->setAccessibleName(label);
		button->setPointerCursor(true);
		button->setClickedCallback([=] {
			if (!i) {
				_controller->widget()->closeAccountManagement();
			} else {
				showPage(static_cast<ManagementPage>(i - 1));
			}
		});
		button->paintRequest() | rpl::on_next([=] {
			auto p = Painter(button);
			const auto active = i == int(_page) + 1;
			p.fillRect(button->rect(), active ? st::lightButtonBgOver
				: button->isOver() ? st::windowBgOver : st::windowBg);
			const auto &icon = i == 0 ? st::managementBackIcon
				: i == 1 ? st::managementAccountsIcon
				: i == 2 ? st::managementProxyIcon : st::managementProtectionIcon;
			const auto color = active ? st::windowActiveTextFg : st::windowSubTextFg;
			const auto left = st::managementPadding;
			icon.paint(p, left, (button->height() - icon.height()) / 2, button->width(), color->c);
			p.setFont(st::normalFont);
			p.setPen(active ? st::windowActiveTextFg : i ? st::windowFg : st::windowSubTextFg);
			const auto textLeft = left + st::managementAvatarSize + st::managementGap / 2;
			p.drawTextLeft(textLeft, (button->height() - st::normalFont->height) / 2, button->width(),
				st::normalFont->elided(label, std::max(0, button->width() - textLeft - left)));
		}, button->lifetime());
	}
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = Painter(this);
		p.fillRect(clip, st::windowBg);
		const auto nav = std::min(st::managementNavigationWidth, width() / 3);
		const auto pad = st::managementPadding;
		p.fillRect(nav - st::lineWidth, 0, st::lineWidth, height(), st::shadowFg);
		p.fillRect(0, st::managementHeaderHeight - st::lineWidth, nav, st::lineWidth, st::shadowFg);
		p.setFont(st::managementTitleFont);
		p.setPen(st::windowFg);
		p.drawTextLeft(pad, (st::managementHeaderHeight - st::managementTitleFont->height) / 2,
			nav, st::managementTitleFont->elided(tr::lng_management_title(tr::now), std::max(0, nav - 2 * pad)));
		if (height() > st::managementHeaderHeight + 5 * st::managementNavigationRowHeight) {
			const auto session = _controller->maybeSession();
			p.setFont(st::managementTableFont);
			if (session) {
				const auto name = tr::lng_management_current_account(tr::now, lt_name, session->user()->name());
				p.drawTextLeft(pad, height() - 3 * pad, nav,
					st::managementTableFont->elided(name, std::max(0, nav - 2 * pad)));
			}
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(pad, height() - 2 * pad, nav,
				st::managementTableFont->elided(tr::lng_management_subtitle(tr::now,
					lt_amount, QString::number(ranges::count_if(Core::App().domain().accounts(), [](const auto &entry) {
						return entry.account->maybeSession() != nullptr;
					}))), std::max(0, nav - 2 * pad)));
		}
	}, lifetime());
	sizeValue() | rpl::on_next([=](QSize size) {
		const auto nav = std::min(st::managementNavigationWidth, size.width() / 3);
		for (auto i = 0; i != int(buttons.size()); ++i) {
			buttons[i]->setGeometry(0, st::managementHeaderHeight + i * st::managementNavigationRowHeight,
				nav - st::lineWidth, st::managementNavigationRowHeight);
		}
		if (_content) {
			_content->setGeometry(nav, 0, size.width() - nav, size.height());
		}
	}, lifetime());
	showPage(proxies ? ManagementPage::Proxies : ManagementPage::Accounts);
}

void Management::showPage(ManagementPage page) {
	_page = page;
	update();
	for (const auto child : children()) {
		if (const auto widget = qobject_cast<QWidget*>(child)) {
			widget->update();
		}
	}
	_content.destroy();
	_content = page == ManagementPage::Protection
		? Settings::CreateProtection(this, _controller)
		: page == ManagementPage::Proxies
		? Settings::CreateProxyManagement(this, _controller)
		: Settings::CreateAccountManagement(this, _controller);
	const auto nav = std::min(st::managementNavigationWidth, width() / 3);
	_content->setGeometry(nav, 0, width() - nav, height());
	setFocusProxy(_content);
	_content->show();
	_content->setFocus();
}

} // namespace

object_ptr<Ui::RpWidget> CreateAccountManagement(
		QWidget *parent,
		not_null<Controller*> controller,
		bool proxies) {
	return object_ptr<Management>(parent, controller, proxies);
}

} // namespace Window
