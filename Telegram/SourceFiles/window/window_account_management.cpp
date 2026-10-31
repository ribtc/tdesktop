#include "window/window_account_management.h"

#include "lang/lang_keys.h"
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
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(this);
		p.fillRect(clip, st::windowBg);
		const auto nav = std::min(st::managementNavigationWidth, width() / 3);
		p.fillRect(nav - st::lineWidth, 0, st::lineWidth, height(), st::shadowFg);
		const auto row = int(_page) + 1;
		p.fillRect(0, row * st::managementControlHeight,
			nav - st::lineWidth, st::managementControlHeight, st::windowBgOver);
	}, lifetime());
	const auto back = Ui::CreateChild<Ui::LinkButton>(
		this, tr::lng_management_back(tr::now));
	const auto accounts = Ui::CreateChild<Ui::LinkButton>(
		this, tr::lng_management_accounts(tr::now));
	const auto proxy = Ui::CreateChild<Ui::LinkButton>(
		this, tr::lng_management_proxies(tr::now));
	const auto protection = Ui::CreateChild<Ui::LinkButton>(
		this, tr::lng_protection_title(tr::now));
	back->setClickedCallback([=] { _controller->widget()->closeAccountManagement(); });
	accounts->setClickedCallback([=] { showPage(ManagementPage::Accounts); });
	proxy->setClickedCallback([=] { showPage(ManagementPage::Proxies); });
	protection->setClickedCallback([=] { showPage(ManagementPage::Protection); });
	sizeValue() | rpl::on_next([=](QSize size) {
		const auto nav = std::min(st::managementNavigationWidth, size.width() / 3);
		const auto pad = st::managementPadding;
		auto y = pad;
		for (const auto button : { back, accounts, proxy, protection }) {
			button->resize(std::min(button->naturalWidth(), nav - 2 * pad), button->height());
			button->move(pad, y);
			y += st::managementControlHeight;
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
