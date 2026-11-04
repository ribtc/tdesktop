#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"

namespace Window {
class Controller;
} // namespace Window

namespace Settings {

[[nodiscard]] object_ptr<Ui::RpWidget> CreateAccountManagement(
	QWidget *parent,
	not_null<Window::Controller*> controller);

} // namespace Settings
