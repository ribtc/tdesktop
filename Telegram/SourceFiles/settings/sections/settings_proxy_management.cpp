#include "settings/sections/settings_proxy_management.h"

#include "boxes/connection_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/proxy_check_queue.h"
#include "core/proxy_import.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/proxy_check.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "window/window_controller.h"

#include <QtCore/QFile>
#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>

#include "styles/style_boxes.h"
#include "styles/style_window.h"

namespace Settings {
namespace {

constexpr auto kPageSize = 50;

Core::ProxyLibrary &Library() {
	return Core::App().settings().proxyLibrary();
}

bool UpdateConnection(const Core::ProxyEntry &edit) {
	const auto current = Library().find(edit.id);
	if (!current || current->revision != edit.revision) {
		return false;
	}
	auto replacement = *current;
	replacement.proxy = edit.proxy;
	replacement.tryIPv6 = edit.tryIPv6;
	return Library().update(replacement);
}

QString TypeText(const MTP::ProxyData &proxy) {
	switch (proxy.type) {
	case MTP::ProxyData::Type::Socks5: return u"SOCKS5"_q;
	case MTP::ProxyData::Type::Mtproto: return u"MTProto"_q;
	case MTP::ProxyData::Type::Http: return u"HTTP"_q;
	case MTP::ProxyData::Type::Web: return u"WEB"_q;
	default: return tr::lng_proxy_management_unsupported(tr::now);
	}
}

QString Endpoint(const MTP::ProxyData &proxy) {
	return (proxy.host.contains(':') ? u"[%1]"_q.arg(proxy.host) : proxy.host)
		+ ':' + QString::number(proxy.port);
}

QStringList BoundAccounts(const std::vector<QString> &ids) {
	auto result = QStringList();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto account = entry.account.get();
		if (std::find(ids.begin(), ids.end(), account->proxyBindingId()) == ids.end()) {
			continue;
		}
		const auto session = account->maybeSession();
		result.push_back(QString::number(entry.index) + u": "_q
			+ (session ? session->user()->name() + u" ("_q
				+ session->user()->phone() + u")"_q
				: tr::lng_account_proxy_title(tr::now)));
	}
	return result;
}

QString ErrorText(Core::ProxyImportError error) {
	switch (error) {
	case Core::ProxyImportError::Format: return tr::lng_proxy_management_error_format(tr::now);
	case Core::ProxyImportError::Protocol: return tr::lng_proxy_management_error_protocol(tr::now);
	case Core::ProxyImportError::Address: return tr::lng_proxy_management_error_address(tr::now);
	case Core::ProxyImportError::Port: return tr::lng_proxy_management_error_port(tr::now);
	case Core::ProxyImportError::Secret: return tr::lng_proxy_management_error_secret(tr::now);
	case Core::ProxyImportError::Duplicate: return tr::lng_proxy_management_error_duplicate(tr::now);
	case Core::ProxyImportError::Limit: return tr::lng_proxy_management_error_limit(tr::now);
	default: return {};
	}
}

rpl::lifetime RunCheck(
		not_null<Main::Account*> account,
		const Core::ProxyEntry &entry,
		Fn<void(int)> done) {
	struct Check {
		MTP::ProxyCheckConnection v4;
		MTP::ProxyCheckConnection v6;
		base::Timer timer;
		Fn<void(int)> done;
	};
	const auto state = std::make_shared<Check>();
	state->done = std::move(done);
	const auto weak = std::weak_ptr<Check>(state);
	const auto finish = [weak](int ping) {
		if (const auto state = weak.lock()) {
			if (const auto done = base::take(state->done)) {
				done(ping);
			}
		}
	};
	state->timer.setCallback([=] { finish(-1); });
	state->timer.callOnce(15000);
	account->lifetime().add([=] { finish(-1); });
	MTP::StartProxyCheck(&account->mtp(), entry.proxy, entry.tryIPv6,
		state->v4, state->v6,
		[=](MTP::details::AbstractConnection *, int ping) { finish(ping); },
		[=](MTP::details::AbstractConnection *raw) {
			if (const auto state = weak.lock()) {
				MTP::DropProxyChecker(state->v4, state->v6, raw);
				if (!MTP::HasProxyCheckers(state->v4, state->v6)) {
					finish(-1);
				}
			}
		});
	return rpl::lifetime([state] {
		state->done = nullptr;
		state->timer.cancel();
		MTP::ResetProxyCheckers(state->v4, state->v6);
	});
}

void PaintColumns(Painter &p, int width, int y, const QStringList &values) {
	const auto pad = st::proxyManagementPadding;
	const auto available = width - 2 * pad;
	p.setFont(st::normalFont);
	p.setPen(st::windowFg);
	const auto weights = std::array{ 24, 10, 11, 17, 8, 13, 10, 7 };
	auto x = pad;
	for (auto i = 0; i != int(weights.size()); ++i) {
		const auto cell = available * weights[i] / 100;
		p.drawText(x, y, st::normalFont->elided(values[i],
			std::max(0, cell - st::proxyManagementGap)));
		x += cell;
	}
}

class ProxyRow final : public Ui::RpWidget {
public:
	ProxyRow(QWidget *parent, Core::ProxyEntry entry, QString status, Fn<void()> actions);
protected:
	void paintEvent(QPaintEvent *event) override;
private:
	Core::ProxyEntry _entry;
	QString _status;
	int _bindings = 0;

};

class ProxyImport final : public Ui::RpWidget {
public:
	ProxyImport(QWidget *parent, not_null<Window::Controller*> controller, Fn<void()> close);
	void clipboard();
	void chooseFile();
protected:
	void keyPressEvent(QKeyEvent *event) override;
	bool focusNextPrevChild(bool next) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;
private:
	void readFile(const QString &path);
	void preview();
	void layout();
	void submit();
	const not_null<Window::Controller*> _controller;
	Fn<void()> _close;
	QString _source;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _hint;
	object_ptr<Ui::Checkbox> _ipv6;
	object_ptr<Ui::FlatLabel> _family;
	object_ptr<Ui::LinkButton> _file;
	object_ptr<Ui::LinkButton> _clipboard;
	object_ptr<Ui::LinkButton> _dismiss;
	object_ptr<Ui::AbstractButton> _drop;
	object_ptr<Ui::LinkButton> _cancel;
	object_ptr<Ui::LinkButton> _submit;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::RpWidget> _body;
	QPointer<Ui::FlatLabel> _preview;

};

class ProxyManagement final : public Ui::RpWidget {
public:
	ProxyManagement(QWidget *parent, not_null<Window::Controller*> controller);
	~ProxyManagement();
private:
	void rebuild();
	void scheduleRebuild();
	void watchAccounts();
	void layout();
	void menu(QString id);
	void edit(QString id);
	void notes(QString id);
	void family(QString id);
	void remove(std::vector<QString> ids);
	void clearLibrary();
	void confirm(std::vector<QString> ids, bool deleting, Fn<void()> done);
	void check(std::vector<QString> ids);
	void openImport(bool clipboard);
	QString status(const Core::ProxyEntry &entry) const;
	Ui::LinkButton *button(QString text, Fn<void()> callback);
	const not_null<Window::Controller*> _controller;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::InputField> _search;
	object_ptr<Ui::FlatLabel> _summary;
	object_ptr<Ui::RpWidget> _header;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::RpWidget> _rows;
	object_ptr<ProxyImport> _import = { nullptr };
	std::vector<Ui::LinkButton*> _tools;
	std::vector<Ui::LinkButton*> _paging;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Core::ProxyCheckQueue _checks;
	QMap<QString, Core::ProxyCheckResult> _results;
	rpl::lifetime _accountsLifetime;
	int _page = 0;
	int _pages = 1;
	bool _scheduled = false;

};

ProxyRow::ProxyRow(QWidget *parent, Core::ProxyEntry entry, QString status, Fn<void()> actions)
: RpWidget(parent)
, _entry(std::move(entry))
, _status(std::move(status))
, _bindings(BoundAccounts({ _entry.id }).size()) {
	const auto description = Endpoint(_entry.proxy) + u"\n"_q
		+ TypeText(_entry.proxy) + u"\n"_q
		+ tr::lng_proxy_management_unknown(tr::now) + u"\n"_q
		+ _entry.proxy.user + u"\n"_q + _status;
	setAccessibleName(description);
	setToolTip(description);
	const auto button = Ui::CreateChild<Ui::LinkButton>(this, u"⋯"_q);
	button->setAccessibleName(tr::lng_proxy_management_edit(tr::now));
	button->setClickedCallback(std::move(actions));
	sizeValue() | rpl::on_next([=](QSize size) {
		button->move(size.width() - st::proxyManagementPadding - button->width(),
			(size.height() - button->height()) / 2);
	}, lifetime());
}

void ProxyRow::paintEvent(QPaintEvent *event) {
	auto p = Painter(this);
	p.fillRect(event->rect(), st::windowBg);
	const auto pad = st::proxyManagementPadding;
	const auto wide = width() >= st::proxyManagementWideTable;
	if (wide) {
		PaintColumns(p, width(), pad, {
			_entry.notes.isEmpty() ? Endpoint(_entry.proxy) : _entry.notes,
			TypeText(_entry.proxy), tr::lng_proxy_management_unknown(tr::now),
			_entry.proxy.host, QString::number(_entry.proxy.port),
			_entry.proxy.user, QString::number(_bindings), QString(),
		});
		p.setPen(st::windowSubTextFg);
		p.drawText(pad, pad + st::normalFont->height,
			st::normalFont->elided(_status, (width() - 2 * pad) * 24 / 100));
	} else {
		const auto leftWidth = std::max(0, width() - 4 * pad);
		p.setFont(st::normalFont);
		p.setPen(st::windowFg);
		p.drawText(pad, pad, st::normalFont->elided(
			_entry.notes.isEmpty() ? Endpoint(_entry.proxy) : _entry.notes, leftWidth));
		p.setPen(st::windowSubTextFg);
		p.drawText(pad, pad + st::normalFont->height, st::normalFont->elided(
			(_entry.notes.isEmpty() ? TypeText(_entry.proxy) : Endpoint(_entry.proxy))
			+ u" · "_q + _status, leftWidth));
		p.drawText(pad, pad + 2 * st::normalFont->height, st::normalFont->elided(
			tr::lng_proxy_management_bindings(tr::now, lt_amount, QString::number(_bindings))
			+ u" · "_q + tr::lng_proxy_management_hidden(tr::now), leftWidth));
	}
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
}

ProxyImport::ProxyImport(QWidget *parent, not_null<Window::Controller*> controller, Fn<void()> close)
: RpWidget(parent)
, _controller(controller)
, _close(std::move(close))
, _title(this, tr::lng_proxy_management_import_title(), st::defaultFlatLabel)
, _hint(this, tr::lng_proxy_management_import_hint(), st::defaultFlatLabel)
, _ipv6(this, tr::lng_proxy_management_family(tr::now), false)
, _family(this, tr::lng_proxy_management_family_hint(), st::defaultFlatLabel)
, _file(this, tr::lng_proxy_management_choose_file(tr::now))
, _clipboard(this, tr::lng_proxy_management_clipboard(tr::now))
, _dismiss(this, u"×"_q)
, _drop(this)
, _cancel(this, tr::lng_cancel(tr::now))
, _submit(this, tr::lng_proxy_management_import_confirm(tr::now))
, _scroll(this, st::defaultScrollArea) {
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);
	_body = _scroll->setOwnedWidget(object_ptr<Ui::RpWidget>(this));
	for (const auto child : std::vector<QWidget*>{ _hint, _ipv6, _family, _drop, _file, _clipboard }) {
		child->setParent(_body);
		child->show();
	}
	_dismiss->setAccessibleName(tr::lng_close(tr::now));
	_dismiss->setClickedCallback(_close);
	_drop->setPointerCursor(true);
	_drop->setClickedCallback([=] { chooseFile(); });
	_drop->setAccessibleName(tr::lng_proxy_management_choose_file(tr::now));
	_drop->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(_drop);
		p.fillRect(_drop->rect(), st::boxBg);
		auto pen = st::windowSubTextFg->p;
		pen.setStyle(Qt::DashLine);
		p.setPen(pen);
		p.drawRect(_drop->rect().adjusted(0, 0, -1, -1));
	}, _drop->lifetime());
	_drop->lower();
	_file->setClickedCallback([=] { chooseFile(); });
	_clipboard->setClickedCallback([=] { clipboard(); });
	_cancel->setClickedCallback(_close);
	_submit->setClickedCallback([=] { submit(); });
	_ipv6->checkedChanges() | rpl::on_next([=] { preview(); }, lifetime());
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(this);
		p.fillRect(clip, st::layerBg);
		const auto x = std::max(0, width() - st::proxyManagementDrawerWidth);
		p.fillRect(QRect(x, 0, width() - x, height()), st::windowBg);
	}, lifetime());
	sizeValue() | rpl::on_next([=] { layout(); }, lifetime());
	preview();
}

void ProxyImport::clipboard() {
	_source = QGuiApplication::clipboard()->text();
	preview();
}

void ProxyImport::chooseFile() {
	FileDialog::GetOpenPath(this, tr::lng_proxy_management_file(tr::now), u"Text (*.txt);;All files (*)"_q,
		crl::guard(this, [=](const FileDialog::OpenResult &result) {
			if (!result.paths.empty()) {
				readFile(result.paths.front());
			} else if (result.remoteContent.size() > 2 * 1024 * 1024) {
				_controller->showToast(tr::lng_proxy_management_file_error(tr::now));
			} else if (!result.remoteContent.isEmpty()) {
				_source = QString::fromUtf8(result.remoteContent);
				preview();
			}
		}));
}

void ProxyImport::readFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) {
		_source.clear();
		preview();
		_controller->showToast(tr::lng_proxy_management_file_error(tr::now));
		return;
	}
	_source = QString::fromUtf8(file.read(2 * 1024 * 1024 + 1));
	preview();
}

void ProxyImport::preview() {
	const auto rows = Core::ParseProxyImport(_source, Library().entries(), _ipv6->checked());
	auto text = QStringList();
	auto valid = 0;
	for (const auto &row : rows) {
		const auto prefix = tr::lng_proxy_management_preview_line(tr::now, lt_amount, QString::number(row.line));
		if (row.error == Core::ProxyImportError::None) {
			++valid;
			text.push_back(prefix + u": "_q + TypeText(row.entry.proxy)
				+ u" "_q + Endpoint(row.entry.proxy) + u" "_q + row.entry.proxy.user);
		} else {
			text.push_back(prefix + u": "_q + ErrorText(row.error));
		}
	}
	_submit->setDisabled(!valid || !Library().writable());
	delete _preview.data();
	_preview = Ui::CreateChild<Ui::FlatLabel>(_body.data(), text.empty()
		? tr::lng_proxy_management_preview_empty(tr::now) : text.join('\n'), st::defaultFlatLabel);
	layout();
}

void ProxyImport::submit() {
	const auto rows = Core::ParseProxyImport(_source, Library().entries(), _ipv6->checked());
	auto added = 0;
	for (const auto &row : rows) {
		if (row.error == Core::ProxyImportError::None
			&& !Library().add(row.entry.proxy, {}, row.entry.tryIPv6).isEmpty()) {
			++added;
		}
	}
	_controller->showToast(tr::lng_proxy_management_import_done(tr::now, lt_amount, QString::number(added)));
	_close();
}

void ProxyImport::layout() {
	const auto pad = st::proxyManagementPadding;
	const auto left = std::max(0, width() - st::proxyManagementDrawerWidth) + pad;
	const auto inner = std::max(1, width() - left - pad);
	_title->resizeToWidth(std::max(1, inner - _dismiss->width() - pad));
	_title->move(left, pad);
	_dismiss->move(width() - pad - _dismiss->width(), pad);
	const auto top = 2 * pad + _title->height();
	const auto bottom = height() - pad - _submit->height();
	_cancel->move(left, bottom);
	_submit->move(width() - pad - _submit->width(), bottom);
	_scroll->setGeometry(left, top, inner, std::max(0, bottom - pad - top));
	auto y = 0;
	_hint->resizeToWidth(inner);
	_hint->move(0, y);
	y += _hint->height() + pad;
	const auto dropHeight = 3 * pad + _file->height() + _clipboard->height();
	_drop->setGeometry(0, y, inner, dropHeight);
	_file->move(std::max(0, (inner - _file->width()) / 2), y + pad);
	_clipboard->move(std::max(0, (inner - _clipboard->width()) / 2),
		y + 2 * pad + _file->height());
	y += dropHeight + pad;
	_ipv6->resizeToWidth(inner);
	_ipv6->move(0, y);
	y += _ipv6->height() + st::proxyManagementGap;
	_family->resizeToWidth(inner);
	_family->move(0, y);
	y += _family->height() + pad;
	if (_preview) {
		_preview->resizeToWidth(inner);
		_preview->move(0, y);
		y += _preview->height();
	}
	_body->resize(inner, y);

}

void ProxyImport::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape) {
		_close();
		event->accept();
	} else {
		RpWidget::keyPressEvent(event);
	}
}

bool ProxyImport::focusNextPrevChild(bool next) {
	auto candidate = focusWidget() ? focusWidget() : this;
	const auto start = candidate;
	do {
		candidate = next ? candidate->nextInFocusChain() : candidate->previousInFocusChain();
		if (isAncestorOf(candidate) && !candidate->isHidden()
			&& candidate->isEnabled() && candidate->focusPolicy() != Qt::NoFocus) {
			candidate->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
			return true;
		}
	} while (candidate != start);
	setFocus();
	return true;
}

void ProxyImport::dragEnterEvent(QDragEnterEvent *event) {
	if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1
		&& event->mimeData()->urls().front().isLocalFile()) {
		event->acceptProposedAction();
	}
}

void ProxyImport::dropEvent(QDropEvent *event) {
	if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1
		&& event->mimeData()->urls().front().isLocalFile()) {
		readFile(event->mimeData()->urls().front().toLocalFile());
		event->acceptProposedAction();
	}
}

ProxyManagement::ProxyManagement(QWidget *parent, not_null<Window::Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _title(this, tr::lng_proxy_management_title(), st::defaultFlatLabel)
, _search(this, st::defaultInputField, tr::lng_proxy_management_search())
, _summary(this, QString(), st::defaultFlatLabel)
, _header(this)
, _scroll(this, st::defaultScrollArea)
, _checks([=](const Core::ProxyEntry &entry, Fn<void(int)> done) {
	return RunCheck(&_controller->account(), entry, std::move(done));
}) {
	paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(this);
		p.fillRect(clip, st::windowBg);
	}, lifetime());
	_header->paintRequest() | rpl::on_next([=] {
		auto p = Painter(_header);
		p.fillRect(_header->rect(), st::boxBg);
		PaintColumns(p, _header->width(), st::proxyManagementPadding, {
			tr::lng_proxy_management_column_notes(tr::now),
			tr::lng_proxy_management_column_type(tr::now),
			tr::lng_proxy_management_column_country(tr::now), u"IP"_q,
			tr::lng_proxy_box_port(tr::now), tr::lng_proxy_box_username(tr::now),
			tr::lng_proxy_management_column_accounts(tr::now),
			tr::lng_proxy_management_column_actions(tr::now),
		});
	}, _header->lifetime());
	_tools.push_back(button(tr::lng_proxy_management_add(tr::now), [=] {
		auto initial = MTP::ProxyData();
		initial.type = MTP::ProxyData::Type::Socks5;
		_controller->show(CreateAccountProxyEditBox(initial, crl::guard(this, [=](MTP::ProxyData proxy) {
			if (Library().add(std::move(proxy)).isEmpty()) {
				_controller->showToast(tr::lng_proxy_management_changed(tr::now));
			}
		})));
	}));
	_tools.push_back(button(tr::lng_proxy_management_file(tr::now), [=] { openImport(false); }));
	_tools.push_back(button(tr::lng_proxy_management_clipboard(tr::now), [=] { openImport(true); }));
	_tools.push_back(button(tr::lng_proxy_management_clear(tr::now), [=] { clearLibrary(); }));
	_tools.push_back(button(tr::lng_proxy_management_check(tr::now), [=] {
		auto ids = std::vector<QString>();
		for (const auto &entry : Library().entries()) {
			ids.push_back(entry.id);
		}
		check(std::move(ids));
	}));
	_tools.push_back(button(tr::lng_proxy_management_cancel_check(tr::now), [=] { _checks.cancel(); }));
	_paging.push_back(button(tr::lng_proxy_management_previous(tr::now), [=] { --_page; rebuild(); }));
	_paging.push_back(button(tr::lng_proxy_management_next(tr::now), [=] { ++_page; rebuild(); }));
	_search->changes() | rpl::on_next([=] { _page = 0; scheduleRebuild(); }, lifetime());
	Library().changes() | rpl::on_next([=](Core::ProxyLibraryChange change) {
		if (change.connection) {
			_checks.cancel();
			_results.remove(change.id);
		}
		if (!change.before) {
			scheduleRebuild();
		}
	}, lifetime());
	_checks.results() | rpl::on_next([=](Core::ProxyCheckResult result) {
		const auto entry = Library().find(result.id);
		if (entry && entry->revision == result.revision) {
			_results[result.id] = result;
			scheduleRebuild();
		}
	}, lifetime());
	Core::App().domain().accountsChanges() | rpl::on_next([=] { watchAccounts(); }, lifetime());
	sizeValue() | rpl::on_next([=] { layout(); }, lifetime());
	watchAccounts();
}

ProxyManagement::~ProxyManagement() {
	_checks.cancel();
}

Ui::LinkButton *ProxyManagement::button(QString text, Fn<void()> callback) {
	const auto result = Ui::CreateChild<Ui::LinkButton>(this, text);
	result->setClickedCallback(std::move(callback));
	return result;
}

void ProxyManagement::watchAccounts() {
	_accountsLifetime.destroy();
	for (const auto &entry : Core::App().domain().accounts()) {
		entry.account->network().changes() | rpl::on_next([=] { scheduleRebuild(); }, _accountsLifetime);
		entry.account->sessionChanges() | rpl::on_next([=] { scheduleRebuild(); }, _accountsLifetime);
	}
	scheduleRebuild();
}

void ProxyManagement::scheduleRebuild() {
	if (_scheduled) {
		return;
	}
	_scheduled = true;
	Ui::PostponeCall(this, [=] { _scheduled = false; rebuild(); });
}

QString ProxyManagement::status(const Core::ProxyEntry &entry) const {
	if (!Core::ProxyLibrary::Supported(entry.proxy)) {
		return tr::lng_proxy_management_unsupported(tr::now);
	}
	const auto i = _results.constFind(entry.id);
	if (i == _results.cend() || i->revision != entry.revision) {
		return tr::lng_proxy_management_untested(tr::now);
	}
	switch (i->state) {
	case Core::ProxyCheckState::Checking: return tr::lng_proxy_management_checking(tr::now);
	case Core::ProxyCheckState::Available:
		return tr::lng_proxy_management_available(
			tr::now,
			lt_amount,
			QString::number(i->ping));
	case Core::ProxyCheckState::Unavailable: return tr::lng_proxy_management_unavailable(tr::now);
	case Core::ProxyCheckState::Cancelled: return tr::lng_proxy_management_cancelled(tr::now);
	}
	return tr::lng_proxy_management_untested(tr::now);
}

void ProxyManagement::rebuild() {
	auto entries = std::vector<Core::ProxyEntry>();
	const auto search = _search->getLastText().trimmed();
	for (const auto &entry : Library().entries()) {
		if (search.isEmpty() || entry.notes.contains(search, Qt::CaseInsensitive)
			|| entry.proxy.host.contains(search, Qt::CaseInsensitive)
			|| entry.proxy.user.contains(search, Qt::CaseInsensitive)) {
			entries.push_back(entry);
		}
	}
	_pages = std::max(1, int((entries.size() + kPageSize - 1) / kPageSize));
	_page = std::clamp(_page, 0, _pages - 1);
	auto rows = object_ptr<Ui::RpWidget>(this);
	_rows = rows.data();
	const auto end = std::min(int(entries.size()), (_page + 1) * kPageSize);
	for (auto i = _page * kPageSize; i < end; ++i) {
		const auto &entry = entries[i];
		const auto row = Ui::CreateChild<ProxyRow>(_rows.data(), entry, status(entry),
			[=, id = entry.id] { menu(id); });
		row->move(0, (i - _page * kPageSize) * st::proxyManagementRowHeight);
		_rows->widthValue() | rpl::on_next([=](int width) {
			row->resize(width, st::proxyManagementRowHeight);
		}, row->lifetime());
	}
	_rows->resize(width(), (end - _page * kPageSize) * st::proxyManagementRowHeight);
	_scroll->setOwnedWidget(std::move(rows));
	_summary->setText(Library().writable()
		? tr::lng_proxy_management_summary(tr::now, lt_amount, QString::number(entries.size()))
			+ u" · "_q + tr::lng_proxy_management_page(tr::now, lt_amount, QString::number(_page + 1))
			+ u" / "_q + QString::number(_pages)
		: tr::lng_proxy_management_readonly(tr::now));
	for (auto i = 0; i != 4; ++i) {
		_tools[i]->setDisabled(!Library().writable());
	}
	_paging[0]->setDisabled(_page == 0);
	_paging[1]->setDisabled(_page + 1 == _pages);
	layout();
}

void ProxyManagement::layout() {
	const auto pad = st::proxyManagementPadding;
	const auto inner = std::max(1, width() - 2 * pad);
	_title->resizeToWidth(inner);
	_title->move(pad, pad);
	auto y = pad + _title->height() + pad;
	auto x = pad;
	for (const auto tool : _tools) {
		if (x > pad && x + tool->width() > width() - pad) {
			x = pad;
			y += st::proxyManagementControlHeight;
		}
		tool->move(x, y);
		x += tool->width() + st::proxyManagementGap;
	}
	y += st::proxyManagementControlHeight;
	_search->resizeToWidth(inner);
	_search->move(pad, y);
	y += _search->height() + pad;
	_summary->resizeToWidth(inner);
	_summary->move(pad, y);
	y += _summary->height() + pad;
	const auto wide = inner >= st::proxyManagementWideTable;
	_header->setVisible(wide);
	if (wide) {
		_header->setGeometry(pad, y, inner, st::proxyManagementControlHeight);
		y += st::proxyManagementControlHeight;
	}
	const auto bottom = height() - pad - st::proxyManagementControlHeight;
	_scroll->setGeometry(pad, y, inner, std::max(0, bottom - y));
	if (_rows) {
		_rows->resize(inner, _rows->height());
	}
	_paging[0]->move(pad, bottom + st::proxyManagementGap);
	_paging[1]->move(width() - pad - _paging[1]->width(), bottom + st::proxyManagementGap);
	if (_import) {
		_import->setGeometry(rect());
	}
}

void ProxyManagement::menu(QString id) {
	const auto entry = Library().find(id);
	if (!entry) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(tr::lng_proxy_management_edit(tr::now), [=] { edit(id); });
	_menu->addAction(tr::lng_proxy_management_notes(tr::now), [=] { notes(id); });
	_menu->addAction(tr::lng_proxy_management_family(tr::now), [=] { family(id); });
	_menu->addAction(tr::lng_proxy_management_check_one(tr::now), [=] { check({ id }); });
	_menu->addAction(tr::lng_proxy_management_delete(tr::now), [=] { remove({ id }); });
	_menu->popup(QCursor::pos());
}

void ProxyManagement::confirm(std::vector<QString> ids, bool deleting, Fn<void()> done) {
	const auto accounts = BoundAccounts(ids);
	_controller->show(Ui::MakeConfirmBox({
		.text = (deleting ? tr::lng_proxy_management_confirm_delete(tr::now)
			: tr::lng_proxy_management_confirm_change(tr::now)) + u"\n\n"_q
			+ (accounts.empty() ? tr::lng_proxy_management_no_accounts(tr::now) : accounts.join('\n')),
		.confirmed = crl::guard(this, [=](Fn<void()> close) {
			close();
			if (BoundAccounts(ids) != accounts) {
				confirm(ids, deleting, done);
			} else {
				done();
			}
		}),
	}));
}

void ProxyManagement::edit(QString id) {
	const auto entry = Library().find(id);
	if (!entry) {
		return;
	}
	const auto saved = *entry;
	auto editable = saved.proxy;
	if (editable.type != MTP::ProxyData::Type::Socks5
		&& editable.type != MTP::ProxyData::Type::Mtproto) {
		editable.type = MTP::ProxyData::Type::Socks5;
	}
	_controller->show(CreateAccountProxyEditBox(editable,
		crl::guard(this, [=](MTP::ProxyData proxy) {
			auto changed = saved;
			changed.proxy = std::move(proxy);
			confirm({ id }, false, [=] {
				if (!UpdateConnection(changed)) {
					_controller->showToast(tr::lng_proxy_management_changed(tr::now));
				}
			});
		})));
}

void ProxyManagement::notes(QString id) {
	const auto entry = Library().find(id);
	if (!entry) {
		return;
	}
	const auto initial = entry->notes;
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_proxy_management_notes());
		const auto input = box->addRow(object_ptr<Ui::InputField>(box,
			st::defaultInputField, rpl::single(QString()), initial));
		box->addButton(tr::lng_settings_save(), crl::guard(this, [=] {
			const auto current = Library().find(id);
			if (!current) {
				box->closeBox();
				return;
			}
			auto changed = *current;
			changed.notes = input->getLastText();
			if (Library().update(changed)) {
				box->closeBox();
			}
		}));
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ProxyManagement::family(QString id) {
	const auto entry = Library().find(id);
	if (!entry) {
		return;
	}
	const auto saved = *entry;
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_proxy_management_family());
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_proxy_management_family_hint(), st::defaultFlatLabel));
		const auto check = box->addRow(object_ptr<Ui::Checkbox>(box,
			tr::lng_proxy_management_family(tr::now), saved.tryIPv6));
		box->addButton(tr::lng_settings_save(), crl::guard(this, [=] {
			auto changed = saved;
			changed.tryIPv6 = check->checked();
			box->closeBox();
			confirm({ id }, false, [=] {
				if (!UpdateConnection(changed)) {
					_controller->showToast(tr::lng_proxy_management_changed(tr::now));
				}
			});
		}));
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ProxyManagement::remove(std::vector<QString> ids) {
	if (ids.empty()) {
		return;
	}
	confirm(ids, true, [=] {
		for (const auto &id : ids) {
			Library().remove(id);
		}
	});
}

void ProxyManagement::clearLibrary() {
	const auto ids = Library().identifiers();
	confirm(ids, true, [=] {
		if (Library().identifiers() != ids) {
			clearLibrary();
		} else {
			Library().clear();
		}
	});
}

void ProxyManagement::check(std::vector<QString> ids) {
	auto entries = std::vector<Core::ProxyEntry>();
	for (const auto &id : ids) {
		const auto entry = Library().find(id);
		if (entry && Core::ProxyLibrary::Supported(entry->proxy)) {
			entries.push_back(*entry);
		}
	}
	_checks.start(std::move(entries));
}

void ProxyManagement::openImport(bool clipboard) {
	_import = object_ptr<ProxyImport>(this, _controller, [=] {
		_import.destroyDelayed();
		_search->setFocus();
	});
	_import->setGeometry(rect());
	_import->show();
	_import->raise();
	_import->setFocus();
	if (clipboard) {
		_import->clipboard();
	}
}

} // namespace

object_ptr<Ui::RpWidget> CreateProxyManagement(
		QWidget *parent,
		not_null<Window::Controller*> controller) {
	return object_ptr<ProxyManagement>(parent, controller);
}

} // namespace Settings
