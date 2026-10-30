#pragma once

#include "ui/rp_widget.h"

namespace Window {

class Controller;

[[nodiscard]] object_ptr<Ui::RpWidget> CreateAccountManagement(
	QWidget *parent,
	not_null<Controller*> controller,
	bool proxies);

} // namespace Window
