#include "main/account_management.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>
#include <algorithm>

namespace Main {
namespace {

bool ValidMetadata(const QJsonObject &root) {
	if (root["version"].toInt() != 1
		|| !root["groups"].isObject()
		|| !root["accounts"].isObject()) {
		return false;
	}
	const auto groups = root["groups"].toObject();
	for (auto i = groups.begin(); i != groups.end(); ++i) {
		if (i.key().isEmpty() || !i.value().isString()
			|| i.value().toString().trimmed().isEmpty()) {
			return false;
		}
	}
	const auto accounts = root["accounts"].toObject();
	for (auto i = accounts.begin(); i != accounts.end(); ++i) {
		const auto entry = i.value().toObject();
		if (i.key().isEmpty() || !entry["group"].isString()
			|| !entry["tags"].isArray()) {
			return false;
		}
		for (const auto &tag : entry["tags"].toArray()) {
			if (!tag.isString()) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

bool AccountMetadata::load(const QByteArray &bytes) {
	_groups.clear();
	_assignments.clear();
	_tags.clear();
	_unreadable.clear();
	if (bytes.isEmpty()) {
		return true;
	}
	const auto document = QJsonDocument::fromJson(bytes);
	const auto root = document.object();
	if (!document.isObject() || !ValidMetadata(root)) {
		_unreadable = bytes;
		return false;
	}
	const auto groups = root["groups"].toObject();
	for (auto i = groups.begin(); i != groups.end(); ++i) {
		_groups.insert(i.key(), i.value().toString());
	}
	const auto accounts = root["accounts"].toObject();
	for (auto i = accounts.begin(); i != accounts.end(); ++i) {
		const auto entry = i.value().toObject();
		assign(i.key(), entry["group"].toString());
		auto tags = QStringList();
		for (const auto &tag : entry["tags"].toArray()) {
			tags.push_back(tag.toString());
		}
		setTags(i.key(), std::move(tags));
	}
	return true;
}

QByteArray AccountMetadata::serialize() const {
	if (!_unreadable.isEmpty()) {
		return _unreadable;
	}
	auto groups = QJsonObject();
	for (auto i = _groups.begin(); i != _groups.end(); ++i) {
		groups.insert(i.key(), i.value());
	}
	auto accounts = QJsonObject();
	for (auto i = _assignments.begin(); i != _assignments.end(); ++i) {
		accounts.insert(i.key(), QJsonObject{
			{ "group", i.value() },
			{ "tags", QJsonArray::fromStringList(tags(i.key())) },
		});
	}
	return QJsonDocument(QJsonObject{
		{ "version", 1 }, { "groups", groups }, { "accounts", accounts },
	}).toJson(QJsonDocument::Compact);
}

QString AccountMetadata::addGroup(QString name) {
	name = name.trimmed();
	if (!_unreadable.isEmpty() || name.isEmpty() || _groups.values().contains(name)) {
		return {};
	}
	const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	_groups.insert(id, std::move(name));
	return id;
}

bool AccountMetadata::renameGroup(const QString &id, QString name) {
	name = name.trimmed();
	if (!_groups.contains(id) || name.isEmpty()
		|| (_groups.values().contains(name) && _groups.value(id) != name)) {
		return false;
	}
	_groups[id] = std::move(name);
	return true;
}

void AccountMetadata::removeGroup(const QString &id) {
	_groups.remove(id);
	for (auto &group : _assignments) {
		if (group == id) {
			group.clear();
		}
	}
}

void AccountMetadata::assign(const QString &account, const QString &group) {
	_assignments[account] = _groups.contains(group) ? group : QString();
}

void AccountMetadata::setTags(const QString &account, QStringList tags) {
	for (auto &tag : tags) {
		tag = tag.trimmed();
	}
	tags.removeAll(QString());
	tags.removeDuplicates();
	_tags[account] = std::move(tags);
	if (!_assignments.contains(account)) {
		_assignments.insert(account, {});
	}
}

QString AccountMetadata::group(const QString &account) const {
	return _assignments.value(account);
}

QStringList AccountMetadata::tags(const QString &account) const {
	return _tags.value(account);
}

const QMap<QString, QString> &AccountMetadata::groups() const {
	return _groups;
}

void AccountSelection::select(const QStringList &ids) {
	_ids.append(ids);
	_ids.removeDuplicates();
}

void AccountSelection::toggle(const QString &id) {
	if (!_ids.removeAll(id)) {
		_ids.push_back(id);
	}
}

void AccountSelection::clear() {
	_ids.clear();
}

bool AccountSelection::contains(const QString &id) const {
	return _ids.contains(id);
}

QStringList AccountSelection::freeze() const {
	return _ids;
}

AccountPage QueryAccounts(
		const std::vector<ManagedAccount> &accounts,
		const AccountMetadata &metadata,
		const AccountQuery &query) {
	auto result = AccountPage();
	const auto search = query.search.trimmed();
	for (const auto &account : accounts) {
		if (query.group && metadata.group(account.id) != *query.group) {
			continue;
		}
		if (query.status && account.status != *query.status) {
			continue;
		}
		if (!search.isEmpty()
			&& !account.name.contains(search, Qt::CaseInsensitive)
			&& !account.phone.contains(search, Qt::CaseInsensitive)
			&& !metadata.tags(account.id).join(' ').contains(search, Qt::CaseInsensitive)) {
			continue;
		}
		result.matching.push_back(account.id);
	}
	result.total = result.matching.size();
	const auto size = std::clamp(query.pageSize, 1, 100);
	result.pages = std::max(1, (result.total + size - 1) / size);
	result.page = std::clamp(query.page, 0, result.pages - 1);
	result.ids = result.matching.mid(result.page * size, size);
	return result;
}

} // namespace Main
