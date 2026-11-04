/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QDataStream>
#include <optional>
#include <span>
#include <vector>

namespace Storage {

struct AccountIndex {
	std::vector<int> indices;
	int active = 0;
};

[[nodiscard]] std::optional<AccountIndex> ReadAccountIndex(QDataStream &stream);
[[nodiscard]] bool WriteAccountIndex(
	QDataStream &stream,
	const AccountIndex &index);
[[nodiscard]] int NextAccountIndex(std::span<const int> indices);

} // namespace Storage
