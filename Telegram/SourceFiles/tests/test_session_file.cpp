#include "main/session_file.h"
#include "main/session_sqlite.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

void Require(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

void Database(const QString &path, const QByteArray &sql) {
	auto raw = static_cast<sqlite3*>(nullptr);
	const auto opened = sqlite3_open_v2(
		path.toUtf8().constData(),
		&raw,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
		nullptr);
	const auto db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(
		raw,
		sqlite3_close);
	Require(opened == SQLITE_OK, "Open the synthetic session fixture.");
	Require(sqlite3_exec(db.get(), sql.constData(), nullptr, nullptr, nullptr)
		== SQLITE_OK, "Prepare the synthetic Telethon database.");
}

void ExpectError(const QString &path, Main::SessionFileError expected) {
	const auto result = Main::ReadSessionFile(path);
	const auto error = std::get_if<Main::SessionFileError>(&result);
	Require(error && *error == expected, "Session file error classification.");
}

QByteArray ReadBytes(const QString &path) {
	auto file = QFile(path);
	Require(file.open(QIODevice::ReadOnly), "Read fixture bytes for comparison.");
	return file.readAll();
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	const auto directory = QTemporaryDir();
	Require(directory.isValid(), "A private temporary directory is available.");
	const auto path = directory.filePath(QString::fromLatin1("broken.session"));
	auto file = QFile(path);
	Require(file.open(QIODevice::WriteOnly), "Create a corrupt SQLite session.");
	const auto corrupt = QByteArray("SQLite format 3\0", 16) + QByteArray(128, 'x');
	Require(file.write(corrupt) == corrupt.size(), "Write the corrupt fixture.");
	file.close();
	const auto result = Main::ReadSessionFile(path);
	const auto error = std::get_if<Main::SessionFileError>(&result);
	Require(error && *error == Main::SessionFileError::Invalid,
		"A damaged SQLite database is invalid, not an unsupported session format.");
	const auto valid = directory.filePath(QString::fromLatin1("valid.session"));
	auto key = QByteArray(256, char(0x5a));
	key[0] = 1;
	key[255] = char(0xfe);
	Database(valid,
		"CREATE TABLE version (version INTEGER PRIMARY KEY);"
		"INSERT INTO version VALUES (7);"
		"CREATE TABLE sessions (dc_id INTEGER PRIMARY KEY, server_address TEXT,"
		"port INTEGER, auth_key BLOB, takeout_id INTEGER);"
		"INSERT INTO sessions VALUES (2, '192.0.2.1', 443, X'"
		+ key.toHex() + "', NULL);");
	const auto before = ReadBytes(valid);
	Require(QFile::setPermissions(valid, QFileDevice::ReadOwner),
		"The parser supports a read-only source session.");
	const auto parsed = Main::ReadSessionFile(valid);
	const auto credentials = std::get_if<Main::SessionCredentials>(&parsed);
	Require(credentials && credentials->dcId == 2
		&& credentials->authKey.front() == 1
		&& credentials->authKey[127] == 0x5a
		&& credentials->authKey.back() == 0xfe,
		"Read a Telethon v7 DC and all 256 key bytes without UI or networking.");
	Require(ReadBytes(valid) == before,
		"Importing preserves the original session byte for byte.");
	Require(QFile::setPermissions(valid,
		QFileDevice::ReadOwner | QFileDevice::WriteOwner), "Restore fixture access.");
	Database(valid,
		"ALTER TABLE sessions ADD COLUMN tmp_auth_key BLOB;"
		"UPDATE version SET version = 8;");
	Require(std::holds_alternative<Main::SessionCredentials>(Main::ReadSessionFile(valid)),
		"Telethon v8 optional temporary keys do not change the permanent authorization.");
	Database(valid, "UPDATE sessions SET auth_key = zeroblob(256);");
	ExpectError(valid, Main::SessionFileError::Unauthorized);
	Database(valid, "UPDATE sessions SET auth_key = X'0102';");
	ExpectError(valid, Main::SessionFileError::Invalid);
	Database(valid, "UPDATE sessions SET auth_key = NULL;");
	ExpectError(valid, Main::SessionFileError::Unauthorized);
	Database(valid, "UPDATE sessions SET auth_key = X'" + key.toHex()
		+ "', dc_id = 99;");
	ExpectError(valid, Main::SessionFileError::Invalid);
	Database(valid,
		"UPDATE sessions SET dc_id = 2;"
		"INSERT INTO sessions SELECT 3, server_address, port, auth_key,"
		"takeout_id, tmp_auth_key FROM sessions;");
	ExpectError(valid, Main::SessionFileError::Invalid);
	Database(valid, "DELETE FROM sessions;");
	ExpectError(valid, Main::SessionFileError::Unauthorized);
	Database(valid, "UPDATE version SET version = 99;");
	ExpectError(valid, Main::SessionFileError::Unsupported);
	ExpectError(directory.filePath(QString::fromLatin1("missing.session")),
		Main::SessionFileError::Unreadable);
	const auto plain = directory.filePath(QString::fromLatin1("text.session"));
	auto text = QFile(plain);
	Require(text.open(QIODevice::WriteOnly) && text.write("not a session") == 13,
		"Create an unsupported format fixture.");
	text.close();
	ExpectError(plain, Main::SessionFileError::Unsupported);
	const auto other = directory.filePath(QString::fromLatin1("other.session"));
	Database(other, "CREATE TABLE unrelated (value TEXT);");
	ExpectError(other, Main::SessionFileError::Unsupported);
	for (const auto &fixture : app.arguments().mid(1)) {
		Require(std::holds_alternative<Main::SessionCredentials>(
			Main::ReadSessionFile(fixture)), "Parse a supplied real session fixture.");
	}
	std::puts("Session file tests passed.");
}
