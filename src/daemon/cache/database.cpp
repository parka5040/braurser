#include <braurser/daemon/cache/database.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace braurser::daemon::cache {
namespace {

void check(int result, sqlite3* database) {
    if (result != SQLITE_OK) {
        throw std::runtime_error("SQLite error " + std::to_string(result) + ": " + sqlite3_errmsg(database));
    }
}

void check_statement(int result, sqlite3_stmt* statement) {
    check(result, sqlite3_db_handle(statement));
}

}

Statement::Statement(sqlite3_stmt* statement) : statement_(statement) {}
Statement::~Statement() { if (statement_) sqlite3_finalize(statement_); }
Statement::Statement(Statement&& other) noexcept : statement_(std::exchange(other.statement_, nullptr)) {}
Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (statement_) sqlite3_finalize(statement_);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

void Statement::bind(int index, std::string_view text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<sqlite3_uint64>::max())) {
        throw std::runtime_error("SQLite text value exceeds bind limit");
    }
    check_statement(sqlite3_bind_text64(statement_, index, text.data(),
                                        static_cast<sqlite3_uint64>(text.size()), SQLITE_TRANSIENT, SQLITE_UTF8), statement_);
}
void Statement::bind(int index, std::int64_t value) {
    check_statement(sqlite3_bind_int64(statement_, index, value), statement_);
}
void Statement::bind(int index, double value) {
    check_statement(sqlite3_bind_double(statement_, index, value), statement_);
}
void Statement::bind_null(int index) {
    check_statement(sqlite3_bind_null(statement_, index), statement_);
}
bool Statement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    check_statement(result, statement_);
    return false;
}
void Statement::reset() {
    check_statement(sqlite3_reset(statement_), statement_);
    check_statement(sqlite3_clear_bindings(statement_), statement_);
}
std::optional<std::string> Statement::text(int index) const {
    if (is_null(index)) return std::nullopt;
    const auto* value = sqlite3_column_text(statement_, index);
    const int bytes = sqlite3_column_bytes(statement_, index);
    if (!value) throw std::runtime_error("SQLite text column unavailable");
    return std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes));
}
std::int64_t Statement::integer(int index) const { return sqlite3_column_int64(statement_, index); }
double Statement::real(int index) const { return sqlite3_column_double(statement_, index); }
bool Statement::is_null(int index) const { return sqlite3_column_type(statement_, index) == SQLITE_NULL; }

Database::Database(const std::filesystem::path& path, int flags) {
    if (path.native().find('\0') != std::string::npos) throw std::runtime_error("SQLite path contains NUL");
    const int result = sqlite3_open_v2(path.c_str(), &handle_, flags | SQLITE_OPEN_NOFOLLOW, nullptr);
    if (result != SQLITE_OK) {
        const std::string error = handle_ ? sqlite3_errmsg(handle_) : "cannot allocate handle";
        if (handle_) sqlite3_close(handle_);
        handle_ = nullptr;
        throw std::runtime_error("SQLite open failed: " + error);
    }
    try {
        check(sqlite3_extended_result_codes(handle_, 1), handle_);
        check(sqlite3_busy_timeout(handle_, 2000), handle_);
        check(sqlite3_db_config(handle_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr), handle_);
        check(sqlite3_db_config(handle_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr), handle_);
        execute("PRAGMA foreign_keys=ON");
        execute("PRAGMA trusted_schema=OFF");
        Fts5SelfCheck::run(*this);
    } catch (...) {
        sqlite3_close(handle_);
        handle_ = nullptr;
        throw;
    }
}
Database::~Database() { if (handle_) sqlite3_close(handle_); }
Database::Database(Database&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (handle_) sqlite3_close(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}
Statement Database::prepare(const char* sql) const {
    sqlite3_stmt* statement = nullptr;
    const int result = sqlite3_prepare_v2(handle_, sql, -1, &statement, nullptr);
    if (result != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        check(result, handle_);
    }
    if (!statement) throw std::runtime_error("SQLite empty statement");
    return Statement(statement);
}
void Database::execute(const char* sql) const {
    auto statement = prepare(sql);
    while (statement.step()) {}
}
bool Database::integrity_check() const {
    auto statement = prepare("PRAGMA integrity_check");
    if (!statement.step()) throw std::runtime_error("SQLite integrity check produced no result");
    const bool valid = statement.text(0) == std::optional<std::string>("ok");
    if (statement.step()) return false;
    return valid;
}
std::int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(handle_); }
sqlite3* Database::handle() const noexcept { return handle_; }

void Fts5SelfCheck::run(const Database& database) {
    try {
        database.execute("CREATE VIRTUAL TABLE temp.fts5_probe USING fts5(x)");
        database.execute("DROP TABLE temp.fts5_probe");
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("FTS5 self-check failed: ") + e.what());
    }
}

Transaction::Transaction(Database& database) : database_(database) { database_.execute("BEGIN IMMEDIATE"); }
Transaction::~Transaction() {
    if (active_) {
        try { database_.execute("ROLLBACK"); } catch (...) {}
    }
}
void Transaction::commit() {
    database_.execute("COMMIT");
    active_ = false;
}

}
