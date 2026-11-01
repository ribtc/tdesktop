#pragma once

#include <memory>

namespace Main {
class Account;
} // namespace Main

namespace Ui {
class Show;
} // namespace Ui

namespace Intro {

void ChooseSessionFiles(
	not_null<QWidget*> parent,
	not_null<Main::Account*> account,
	std::shared_ptr<Ui::Show> show);

} // namespace Intro
