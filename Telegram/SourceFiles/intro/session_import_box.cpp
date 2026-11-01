#include "intro/session_import_box.h"

#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/session_import.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/labels.h"
#include <crl/crl_on_main.h>
#include <QtCore/QFileInfo>

#include "styles/style_layers.h"

namespace Intro {
namespace {

constexpr auto kMaxBatchSize = 100;

QString StatusText(Main::SessionImportStatus status) {
	using Status = Main::SessionImportStatus;
	switch (status) {
	case Status::Waiting: return tr::lng_session_waiting(tr::now);
	case Status::Reading: return tr::lng_session_reading(tr::now);
	case Status::Connecting: return tr::lng_session_connecting(tr::now);
	case Status::Imported: return tr::lng_session_imported(tr::now);
	case Status::Duplicate: return tr::lng_session_duplicate(tr::now);
	case Status::Unreadable: return tr::lng_session_unreadable(tr::now);
	case Status::Unsupported: return tr::lng_session_unsupported(tr::now);
	case Status::Invalid: return tr::lng_session_invalid(tr::now);
	case Status::Unauthorized: return tr::lng_session_unauthorized(tr::now);
	case Status::NetworkError: return tr::lng_session_network_error(tr::now);
	case Status::Cancelled: return tr::lng_session_cancelled(tr::now);
	}
	Unexpected("Session import status");
}

void ImportBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Account*> account,
		QStringList paths) {
	box->setTitle(tr::lng_session_login());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_session_import_about(),
		st::boxLabel));
	const auto importer = box->lifetime().make_state<Main::SessionImport>(account);
	auto labels = std::vector<not_null<Ui::FlatLabel*>>();
	for (const auto &path : paths) {
		labels.push_back(box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QFileInfo(path).fileName() + u": "_q
				+ StatusText(Main::SessionImportStatus::Waiting),
			st::boxLabel)));
	}
	importer->updates() | rpl::on_next([=](Main::SessionImportUpdate update) {
		labels[update.index]->setText(QFileInfo(paths[update.index]).fileName()
			+ u": "_q + StatusText(update.status));
	}, box->lifetime());
	const auto activate = !account->sessionExists();
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	box->boxClosing() | rpl::on_next([=] {
		importer->cancel();
		const auto imported = importer->firstImported();
		if (activate && imported) {
			crl::on_main(imported, [=] { imported->domain().activate(imported); });
		}
	}, box->lifetime());
	importer->start(std::move(paths));
}

} // namespace

void ChooseSessionFiles(
		not_null<QWidget*> parent,
		not_null<Main::Account*> account,
		std::shared_ptr<Ui::Show> show) {
	const auto weak = base::make_weak(account);
	FileDialog::GetOpenPaths(
		parent.get(),
		tr::lng_session_login(tr::now),
		u"Telethon Session (*.session)"_q,
		crl::guard(parent, [=](FileDialog::OpenResult &&result) {
			if (!weak || result.paths.isEmpty()) {
				return;
			} else if (result.paths.size() > kMaxBatchSize) {
				show->show(Ui::MakeInformBox(tr::lng_session_batch_limit()));
				return;
			}
			show->show(Box(ImportBox, weak.get(), std::move(result.paths)));
		}));
}

} // namespace Intro
