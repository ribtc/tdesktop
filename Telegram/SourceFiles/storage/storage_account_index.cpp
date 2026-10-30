/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_account_index.h"

#include <QtCore/QIODevice>
#include <limits>
#include <set>

namespace Storage {
namespace {

[[nodiscard]] bool ValidIndex(int index) {
	return index >= 0 && index < std::numeric_limits<int>::max();
}

} // namespace

std::optional<AccountIndex> ReadAccountIndex(QDataStream &stream) {
	auto count = qint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok
		|| count <= 0
		|| !stream.device()
		|| count > stream.device()->bytesAvailable() / sizeof(qint32)) {
		return std::nullopt;
	}
	auto result = AccountIndex();
	auto seen = std::set<int>();
	for (auto i = 0; i != count; ++i) {
		auto index = qint32();
		stream >> index;
		if (!ValidIndex(index) || !seen.emplace(index).second) {
			return std::nullopt;
		}
		result.indices.push_back(index);
	}
	result.active = result.indices.front();
	if (!stream.atEnd()) {
		auto active = qint32();
		stream >> active;
		if (seen.contains(active)) {
			result.active = active;
		}
	}
	return (stream.status() == QDataStream::Ok)
		? std::make_optional(std::move(result))
		: std::nullopt;
}

bool WriteAccountIndex(QDataStream &stream, const AccountIndex &index) {
	if (index.indices.empty()
		|| index.indices.size() > std::numeric_limits<qint32>::max()) {
		return false;
	}
	auto seen = std::set<int>();
	for (const auto value : index.indices) {
		if (!ValidIndex(value) || !seen.emplace(value).second) {
			return false;
		}
	}
	stream << qint32(index.indices.size());
	for (const auto value : index.indices) {
		stream << qint32(value);
	}
	stream << qint32(index.active);
	return stream.status() == QDataStream::Ok;
}

int NextAccountIndex(std::span<const int> indices) {
	const auto used = std::set<int>(indices.begin(), indices.end());
	auto result = 0;
	for (const auto index : used) {
		if (index == result) {
			++result;
		} else if (index > result) {
			break;
		}
	}
	return ValidIndex(result) ? result : -1;
}

} // namespace Storage
