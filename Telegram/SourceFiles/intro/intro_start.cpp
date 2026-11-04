/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_start.h"

#include "boxes/abstract_box.h"
#include "boxes/connection_box.h"
#include "core/version.h"
#include "intro/intro_phone.h"
#include "intro/intro_qr.h"
#include "intro/intro_widget.h"
#include "intro/session_import_box.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "mtproto/network_context.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"

#include "styles/style_intro.h"

namespace Intro {
namespace details {

StartWidget::StartWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data, true) {
	setMouseTracking(true);
	setTitleText(rpl::single(AppDisplayName.utf16()));
	setDescriptionText(tr::lng_intro_network_about());
	const auto session = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_session_login(tr::now));
	rpl::combine(sizeValue(), session->widthValue()) | rpl::on_next([=](
			QSize size,
			int width) {
		session->moveToLeft(
			(size.width() - width) / 2,
			contentTop() + st::introSessionTop);
	}, session->lifetime());
	session->setClickedCallback([=] {
		ChooseSessionFiles(this, account, data->controller->uiShow());
	});
	show();
}

void StartWidget::submit() {
	const auto state = account().network().snapshot().state;
	if (state == MTP::NetworkState::Unbound
		|| state == MTP::NetworkState::Failed) {
		Ui::show(CreateAccountProxyBox(&account()));
		return;
	}
	account().destroyStaleAuthorizationKeys();
	goNext<QrWidget>();
}

rpl::producer<QString> StartWidget::nextButtonText() const {
	return tr::lng_start_msgs();
}

rpl::producer<> StartWidget::nextButtonFocusRequests() const {
	return _nextButtonFocusRequests.events();
}

void StartWidget::activate() {
	Step::activate();
	setInnerFocus();
}

void StartWidget::setInnerFocus() {
	_nextButtonFocusRequests.fire({});
}

} // namespace details
} // namespace Intro
