#pragma once

#include "core/proxy_library.h"

namespace Core {

enum class ProxyImportError {
	None,
	Format,
	Protocol,
	Address,
	Port,
	Secret,
	Duplicate,
	Limit,
};

struct ProxyImportRow {
	ProxyEntry entry;
	int line = 0;
	ProxyImportError error = ProxyImportError::None;
};

[[nodiscard]] std::vector<ProxyImportRow> ParseProxyImport(
	const QString &text,
	const std::vector<ProxyEntry> &existing,
	bool tryIPv6);

} // namespace Core
