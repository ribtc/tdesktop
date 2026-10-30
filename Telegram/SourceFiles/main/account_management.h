#pragma once

#include <QtCore/QMap>
#include <QtCore/QStringList>
#include <optional>
#include <vector>

namespace Main {

enum class AccountStatus { Ready, Unbound, Checking, Failed };

struct ManagedAccount {
	QString id;
	QString name;
	QString phone;
	AccountStatus status = AccountStatus::Unbound;
};

class AccountMetadata final {
public:
	[[nodiscard]] bool load(const QByteArray &bytes);
	[[nodiscard]] QByteArray serialize() const;
	[[nodiscard]] QString addGroup(QString name);
	bool renameGroup(const QString &id, QString name);
	void removeGroup(const QString &id);
	void assign(const QString &account, const QString &group);
	void setTags(const QString &account, QStringList tags);
	[[nodiscard]] QString group(const QString &account) const;
	[[nodiscard]] QStringList tags(const QString &account) const;
	[[nodiscard]] const QMap<QString, QString> &groups() const;

private:
	QMap<QString, QString> _groups;
	QMap<QString, QString> _assignments;
	QMap<QString, QStringList> _tags;
	QByteArray _unreadable;

};

class AccountSelection final {
public:
	void select(const QStringList &ids);
	void toggle(const QString &id);
	void clear();
	[[nodiscard]] bool contains(const QString &id) const;
	[[nodiscard]] QStringList freeze() const;

private:
	QStringList _ids;

};

struct AccountQuery {
	QString search;
	std::optional<QString> group;
	std::optional<AccountStatus> status;
	int page = 0;
	int pageSize = 10;
};

struct AccountPage {
	QStringList ids;
	QStringList matching;
	int total = 0;
	int page = 0;
	int pages = 1;
};

[[nodiscard]] AccountPage QueryAccounts(
	const std::vector<ManagedAccount> &accounts,
	const AccountMetadata &metadata,
	const AccountQuery &query);

} // namespace Main
