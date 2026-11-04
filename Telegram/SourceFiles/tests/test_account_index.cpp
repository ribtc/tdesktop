#include "storage/storage_account_index.h"

#include <QtCore/QBuffer>
#include <QtCore/QTemporaryFile>
#include <cassert>
#include <numeric>

int main() {
	const auto bytes = QByteArray::fromHex(
		"00000007000000000000000100000002000000030000000400000005"
		"0000000600000006");
	auto input = QDataStream(bytes);
	const auto restored = Storage::ReadAccountIndex(input);
	assert(restored);
	assert((restored->indices == std::vector<int>{ 0, 1, 2, 3, 4, 5, 6 }));
	assert(restored->active == 6);
	auto output = QByteArray();
	auto stream = QDataStream(&output, QIODevice::WriteOnly);
	assert(Storage::WriteAccountIndex(stream, *restored));
	assert(output == bytes);
	assert(Storage::NextAccountIndex(restored->indices) == 7);
	auto remaining = restored->indices;
	remaining.erase(remaining.begin() + 3);
	assert(Storage::NextAccountIndex(remaining) == 3);
	remaining.push_back(Storage::NextAccountIndex(remaining));
	assert(Storage::NextAccountIndex(remaining) == 7);
	assert((remaining == std::vector<int>{ 0, 1, 2, 4, 5, 6, 3 }));

	auto large = Storage::AccountIndex{ .active = 499 };
	large.indices.resize(500);
	std::iota(large.indices.begin(), large.indices.end(), 0);
	auto file = QTemporaryFile();
	assert(file.open());
	auto disk = QDataStream(&file);
	assert(Storage::WriteAccountIndex(disk, large));
	assert(file.flush());
	assert(file.seek(0));
	const auto fromDisk = Storage::ReadAccountIndex(disk);
	assert(fromDisk);
	assert(fromDisk->indices.size() == 500);
	assert(fromDisk->indices.front() == 0);
	assert(fromDisk->indices.back() == 499);
	assert(fromDisk->active == 499);
	assert(Storage::NextAccountIndex(fromDisk->indices) == 500);

	for (const auto bad : {
		"00000000",
		"ffffffff",
		"7fffffff00000000",
		"0000000200000000",
		"000000020000000100000001",
		"00000001ffffffff",
		"000000017fffffff",
		"00000001000000000000",
	}) {
		auto input = QDataStream(QByteArray::fromHex(bad));
		assert(!Storage::ReadAccountIndex(input));
	}
	for (const auto legacy : {
		"00000001000001f3",
		"00000001000001f3ffffffff",
	}) {
		auto input = QDataStream(QByteArray::fromHex(legacy));
		const auto index = Storage::ReadAccountIndex(input);
		assert(index && index->active == 499);
	}
	const auto sparse = std::vector<int>{ 0, 2, 499, 2147483646 };
	assert(Storage::NextAccountIndex(sparse) == 1);
}
