#include "main/session_file.h"

#include "main/session_sqlite.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <algorithm>
#include <memory>
#include <optional>

namespace Main {
namespace {

constexpr auto kMaxFileSize = qint64(64 * 1024 * 1024);
constexpr auto kMaxSqlSteps = 10000;

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3 *db, const char *sql) {
	auto raw = static_cast<sqlite3_stmt*>(nullptr);
	const auto result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
	auto statement = Statement(raw, sqlite3_finalize);
	return result == SQLITE_OK ? std::move(statement)
		: Statement(nullptr, sqlite3_finalize);
}

std::optional<bool> IsTable(sqlite3 *db, const char *name) {
	auto query = Prepare(db,
		"SELECT type FROM sqlite_master WHERE name = ? LIMIT 2");
	if (!query || sqlite3_bind_text(query.get(), 1, name, -1, SQLITE_STATIC)
		!= SQLITE_OK) {
		return std::nullopt;
	}
	const auto step = sqlite3_step(query.get());
	if (step == SQLITE_DONE) {
		return false;
	} else if (step != SQLITE_ROW) {
		return std::nullopt;
	}
	const auto type = sqlite3_column_text(query.get(), 0);
	return type && QByteArray(reinterpret_cast<const char*>(type)) == "table"
		&& sqlite3_step(query.get()) == SQLITE_DONE;
}

} // namespace

SessionFileResult ReadSessionFile(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.isFile() || info.size() > kMaxFileSize) {
		return SessionFileError::Unreadable;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return SessionFileError::Unreadable;
	}
	if (file.read(16) != QByteArray("SQLite format 3\0", 16)) {
		return SessionFileError::Unsupported;
	}
	file.close();
	auto raw = static_cast<sqlite3*>(nullptr);
	const auto opened = sqlite3_open_v2(
		info.absoluteFilePath().toUtf8().constData(),
		&raw,
		SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE,
		nullptr);
	auto db = Database(raw, sqlite3_close);
	if (opened != SQLITE_OK) {
		return SessionFileError::Unreadable;
	}
	sqlite3_limit(db.get(), SQLITE_LIMIT_LENGTH, 1024 * 1024);
	sqlite3_limit(db.get(), SQLITE_LIMIT_SQL_LENGTH, 4096);
	sqlite3_limit(db.get(), SQLITE_LIMIT_COLUMN, 64);
	auto steps = 0;
	sqlite3_progress_handler(db.get(), 100, [](void *context) {
		return int(++*static_cast<int*>(context) > kMaxSqlSteps);
	}, &steps);
	const auto hasVersion = IsTable(db.get(), "version");
	const auto hasSessions = IsTable(db.get(), "sessions");
	if (!hasVersion || !hasSessions) {
		return SessionFileError::Invalid;
	} else if (!*hasVersion || !*hasSessions) {
		return SessionFileError::Unsupported;
	}
	auto version = Prepare(db.get(), "SELECT version FROM version LIMIT 2");
	if (!version || sqlite3_step(version.get()) != SQLITE_ROW
		|| sqlite3_column_type(version.get(), 0) != SQLITE_INTEGER) {
		return SessionFileError::Invalid;
	}
	const auto number = sqlite3_column_int64(version.get(), 0);
	if (number < 1 || number > 8) {
		return SessionFileError::Unsupported;
	} else if (sqlite3_step(version.get()) != SQLITE_DONE) {
		return SessionFileError::Invalid;
	}
	auto query = Prepare(db.get(),
		"SELECT dc_id, auth_key FROM sessions LIMIT 2");
	if (!query) {
		return SessionFileError::Invalid;
	}
	const auto row = sqlite3_step(query.get());
	if (row == SQLITE_DONE) {
		return SessionFileError::Unauthorized;
	} else if (row != SQLITE_ROW
		|| sqlite3_column_type(query.get(), 0) != SQLITE_INTEGER) {
		return SessionFileError::Invalid;
	}
	const auto dc = sqlite3_column_int64(query.get(), 0);
	const auto length = sqlite3_column_bytes(query.get(), 1);
	if (!length) {
		return SessionFileError::Unauthorized;
	} else if (dc < 1 || dc > 5
		|| length != 256
		|| sqlite3_column_type(query.get(), 1) != SQLITE_BLOB) {
		return SessionFileError::Invalid;
	}
	const auto key = static_cast<const unsigned char*>(
		sqlite3_column_blob(query.get(), 1));
	if (!key) {
		return SessionFileError::Invalid;
	}
	auto credentials = SessionCredentials{ .dcId = int(dc) };
	std::copy_n(key, credentials.authKey.size(), credentials.authKey.begin());
	if (sqlite3_step(query.get()) != SQLITE_DONE) {
		return SessionFileError::Invalid;
	} else if (std::all_of(
		credentials.authKey.begin(),
		credentials.authKey.end(),
		[](unsigned char value) { return value == 0; })) {
		return SessionFileError::Unauthorized;
	}
	return credentials;
}

} // namespace Main
