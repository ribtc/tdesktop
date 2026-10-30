/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

namespace Window {

class Controller;

class AccountsRail final : public Ui::RpWidget {
public:
	AccountsRail(QWidget *parent, not_null<Controller*> controller);

};

} // namespace Window
