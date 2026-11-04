#include "main/account_management.h"

#include <cassert>

int main() {
	auto metadata = Main::AccountMetadata();
	const auto group = metadata.addGroup(QString("Support"));
	assert(!group.isEmpty());
	metadata.assign(QString("100"), group);
	metadata.setTags(QString("100"), { QString("Night") });
	const auto rows = std::vector<Main::ManagedAccount>{
		{ "100", "Alice", "+123", Main::AccountStatus::Ready },
		{ "200", "Bob", "+456", Main::AccountStatus::Unbound },
	};
	auto query = Main::AccountQuery();
	query.search = "night";
	query.group = group;
	query.status = Main::AccountStatus::Ready;
	const auto page = Main::QueryAccounts(rows, metadata, query);
	assert(page.total == 1);
	assert(page.ids == QStringList{ "100" });
	auto selection = Main::AccountSelection();
	selection.select(page.ids);
	const auto frozen = selection.freeze();
	selection.select({ "200" });
	selection.toggle("100");
	assert(frozen == QStringList{ "100" });
	assert(selection.freeze() == QStringList{ "200" });
	auto restored = Main::AccountMetadata();
	assert(restored.load(metadata.serialize()));
	assert(restored.group("100") == group);
	assert(restored.tags("100") == QStringList{ "Night" });
	assert(restored.group("200").isEmpty());
	assert(restored.renameGroup(group, "Support 2"));
	restored.removeGroup(group);
	assert(restored.group("100").isEmpty());
	assert(restored.tags("100") == QStringList{ "Night" });
	const auto malformed = QByteArray(R"({"version":1,"groups":42,"accounts":[]})");
	assert(!restored.load(malformed));
	assert(restored.serialize() == malformed);
	const auto invalidTags = QByteArray(
		R"({"version":1,"groups":{},"accounts":{"100":{"group":"","tags":[42]}}})");
	assert(!restored.load(invalidTags));
	assert(restored.serialize() == invalidTags);
	auto large = std::vector<Main::ManagedAccount>();
	for (auto i = 0; i != 500; ++i) {
		large.push_back({ QString::number(i), QString("Name"),
			QString::number(10000 + i), Main::AccountStatus::Ready });
	}
	query = {};
	query.page = 49;
	const auto last = Main::QueryAccounts(large, metadata, query);
	assert(last.total == 500 && last.pages == 50 && last.page == 49);
	assert(last.ids.front() == "490" && last.ids.back() == "499");
	query.search = "10499";
	const auto narrowed = Main::QueryAccounts(large, metadata, query);
	assert(narrowed.page == 0 && narrowed.total == 1);
	assert(narrowed.ids == QStringList{ "499" });
	selection.clear();
	selection.select(last.ids);
	const auto currentPage = selection.freeze();
	selection.select(last.matching);
	assert(currentPage.size() == 10 && selection.freeze().size() == 500);
	query.search = "missing";
	const auto empty = Main::QueryAccounts(large, metadata, query);
	assert(empty.total == 0 && empty.pages == 1 && empty.page == 0);
}
