#pragma once

#include "ui/rp_widget.h"

namespace Window {
class Controller;
} // namespace Window

namespace Settings {

[[nodiscard]] object_ptr<Ui::RpWidget> CreateProtection(
	QWidget *parent,
	not_null<Window::Controller*> controller);

} // namespace Settings
