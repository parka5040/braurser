#ifndef BRAURSER_DAEMON_CACHE_DATABASE_HPP
#define BRAURSER_DAEMON_CACHE_DATABASE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace braurser::daemon::cache {

class Statement {
public:
    explicit Statement(sqlite3_stmt* statement);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    /// in: index starts at one; text must fit SQLite's bound value limit.
    /// err: throws std::runtime_error on a SQLite error.
    void bind(int index, std::string_view text);
    /// in: index starts at one.
    /// err: throws std::runtime_error on a SQLite error.
    void bind(int index, std::int64_t value);
    /// in: index starts at one.
    /// err: throws std::runtime_error on a SQLite error.
    void bind(int index, double value);
    /// in: index starts at one.
    /// err: throws std::runtime_error on a SQLite error.
    void bind_null(int index);
    /// out: true for a row, false when exhausted.
    /// err: throws std::runtime_error on a SQLite error.
    bool step();
    /// out: resets the statement and clears previous bindings.
    /// err: throws std::runtime_error on a SQLite error.
    void reset();
    /// in: index starts at zero.
    /// out: a copy of the text value, or null.
    std::optional<std::string> text(int index) const;
    /// in: index starts at zero.
    /// out: a signed 64-bit column value.
    std::int64_t integer(int index) const;
    /// in: index starts at zero.
    /// out: a floating-point column value.
    double real(int index) const;
    /// in: index starts at zero.
    /// out: whether the column is SQL NULL.
    bool is_null(int index) const;

private:
    sqlite3_stmt* statement_{};
};

class Database {
public:
    /// in: flags include SQLITE_OPEN_READONLY or SQLITE_OPEN_READWRITE.
    /// err: throws std::runtime_error if opening or the FTS5 self-check fails.
    Database(const std::filesystem::path& path, int flags);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    /// in: sql is a constant SQL statement without variable inputs.
    /// out: a reusable prepared statement.
    Statement prepare(const char* sql) const;
    /// in: sql is constant SQL without variable inputs.
    /// err: throws std::runtime_error on a SQLite error.
    void execute(const char* sql) const;
    /// out: true only if SQLite reports ok.
    /// err: throws std::runtime_error on a SQLite error.
    bool integrity_check() const;
    /// out: the last inserted row id.
    std::int64_t last_insert_id() const;
    /// out: the underlying handle for SQLite diagnostics.
    sqlite3* handle() const noexcept;

private:
    sqlite3* handle_{};
};

struct Fts5SelfCheck {
    /// in: database is an open SQLite connection.
    /// out: FTS5 creates a temporary virtual table successfully.
    /// err: throws std::runtime_error with a clear FTS5 error on failure.
    static void run(const Database& database);
};

class Transaction {
public:
    /// in: database is open and writable.
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    /// out: commits the transaction.
    /// err: throws std::runtime_error on a SQLite error.
    void commit();

private:
    Database& database_;
    bool active_{true};
};

}

#endif
