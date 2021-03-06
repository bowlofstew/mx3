#include "db.hpp"
#include <sqlite3/sqlite3.h>

using mx3::sqlite::Db;
using mx3::sqlite::Stmt;
using mx3::sqlite::ChangeType;

shared_ptr<Db>
Db::open(const string& db_path) {
    constexpr const int flags =
        SQLITE_OPEN_READWRITE |
        SQLITE_OPEN_CREATE |
        // multi-threaded mode
        SQLITE_OPEN_NOMUTEX |
        SQLITE_OPEN_PRIVATECACHE;

    sqlite3 * db;
    auto error_code = sqlite3_open_v2(db_path.c_str(), &db, flags, nullptr);
    auto temp_db = unique_ptr<sqlite3, Db::Closer> {db};

    if (error_code != SQLITE_OK) {
        throw std::runtime_error { sqlite3_errstr(error_code) };
    }
    return shared_ptr<Db> { new Db{std::move(temp_db)} };
}

shared_ptr<Db>
Db::open_memory() {
    return Db::open(":memory:");
}

shared_ptr<Db>
Db::inherit_db(sqlite3 * db) {
    auto temp_db = unique_ptr<sqlite3, Db::Closer> {db};
    return shared_ptr<Db> { new Db{std::move(temp_db)} };
}

Db::Db(unique_ptr<sqlite3, Closer> db) : m_db { std::move(db) } {}

sqlite3 *
Db::borrow_db() {
    return m_db.get();
}

void
Db::update_hook(UpdateHookFn update_fn) {
    m_update_hook = update_fn;
    sqlite3_update_hook(m_db.get(), [] (void * self, int change_type, const char * db_name, const char * table_name, sqlite3_int64 row_id) {
        Db * db = static_cast<Db*>(self);
        if (db->m_update_hook) {
            auto update_hook = db->m_update_hook;
            optional<ChangeType> type = nullopt;
            switch (change_type) {
                case SQLITE_INSERT: {
                    type = ChangeType::INSERT;
                    break;
                }
                case SQLITE_UPDATE: {
                    type = ChangeType::UPDATE;
                    break;
                }
                case SQLITE_DELETE: {
                    type = ChangeType::DELETE;
                    break;
                }
            }
            if (!type) {
                throw std::runtime_error {"Unexpected update type from sqlite"};
            }
            update_hook(*type, string {db_name}, string {table_name}, static_cast<int64_t>(row_id));
        }
    }, this);
}

void
Db::commit_hook(function<bool()> commit_fn) {
    m_commit_hook = commit_fn;
    sqlite3_commit_hook(m_db.get(), [] (void * self) -> int {
        Db * db = static_cast<Db*>(self);
        if (db->m_commit_hook) {
            auto commit_hook = db->m_commit_hook;
            bool result = commit_hook();
            return result ? 0 : 1;
        }
        return 0;
    }, this);
}

void
Db::rollback_hook(function<void()> rollback_fn) {
    m_rollback_hook = rollback_fn;
    sqlite3_rollback_hook(m_db.get(), [] (void * self) {
        Db * db = static_cast<Db*>(self);
        if (db->m_rollback_hook) {
            auto rollback_hook = db->m_rollback_hook;
            rollback_hook();
        }
    } , this);
}

int64_t
Db::last_insert_rowid() {
    return sqlite3_last_insert_rowid(m_db.get());
}

void
Db::Closer::operator() (sqlite3 * db) {
    sqlite3_close_v2(db);
}

void
Db::exec(const string& sql) {
    char * error_msg = nullptr;
    auto result = sqlite3_exec(m_db.get(), sql.c_str(), nullptr, nullptr, &error_msg);
    if (result && error_msg) {
        if (error_msg) {
            throw std::runtime_error { string(error_msg) };
        } else {
            throw std::runtime_error { "Unknown error" };
        }
    }
}

shared_ptr<Stmt>
Db::prepare(const string& sql) {
    sqlite3_stmt * stmt = nullptr;
    const char * end_point = nullptr;
    auto error_code = sqlite3_prepare_v2(m_db.get(), sql.c_str(), static_cast<int>(sql.length()), &stmt, &end_point);

    // we don't use make_shared here since we want to be able hide the ctor
    auto raw_stmt = new Stmt {stmt, this->shared_from_this()};
    shared_ptr<Stmt> wrapped_stmt {raw_stmt};

    if (error_code != SQLITE_OK) {
        throw std::runtime_error { sqlite3_errstr(error_code) };
    } else {
        return wrapped_stmt;
    }
}
