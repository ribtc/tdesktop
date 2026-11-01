#pragma once

#include <sqlite3.h>

#ifdef TDESKTOP_SESSION_TDSQLITE
using sqlite3 = tdsqlite3;
using sqlite3_stmt = tdsqlite3_stmt;
inline constexpr auto &sqlite3_open_v2 = tdsqlite3_open_v2;
inline constexpr auto &sqlite3_close = tdsqlite3_close;
inline constexpr auto &sqlite3_prepare_v2 = tdsqlite3_prepare_v2;
inline constexpr auto &sqlite3_finalize = tdsqlite3_finalize;
inline constexpr auto &sqlite3_bind_text = tdsqlite3_bind_text;
inline constexpr auto &sqlite3_step = tdsqlite3_step;
inline constexpr auto &sqlite3_column_text = tdsqlite3_column_text;
inline constexpr auto &sqlite3_column_type = tdsqlite3_column_type;
inline constexpr auto &sqlite3_column_int64 = tdsqlite3_column_int64;
inline constexpr auto &sqlite3_column_bytes = tdsqlite3_column_bytes;
inline constexpr auto &sqlite3_column_blob = tdsqlite3_column_blob;
inline constexpr auto &sqlite3_limit = tdsqlite3_limit;
inline constexpr auto &sqlite3_progress_handler = tdsqlite3_progress_handler;
inline constexpr auto &sqlite3_exec = tdsqlite3_exec;
#endif // TDESKTOP_SESSION_TDSQLITE
