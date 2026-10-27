#include "db/storage_engine.hpp"
#include "common/utils.hpp"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace grocycle::db {

StorageEngine::StorageEngine(std::string db_path) : db_path_(std::move(db_path)) {}

StorageEngine::~StorageEngine() {
    close();
}

void StorageEngine::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool StorageEngine::execute_sql(const std::string &sql) {
    char *err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            std::cerr << "SQLite error: " << err_msg << " in query: " << sql << std::endl;
            sqlite3_free(err_msg);
        }
        return false;
    }
    return true;
}

bool StorageEngine::initialize() {
    utils::ensure_parent_dir(db_path_);

    int rc = sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open SQLite database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Set Pragmas for performance and concurrency
    execute_sql("PRAGMA journal_mode = WAL;");
    execute_sql("PRAGMA synchronous = NORMAL;");
    execute_sql("PRAGMA foreign_keys = ON;");
    execute_sql("PRAGMA temp_store = MEMORY;");
    execute_sql("PRAGMA busy_timeout = 5000;"); // 5 second timeout on locks

    const std::string schema = R"(
        CREATE TABLE IF NOT EXISTS monitored_roots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS snapshots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            root_id INTEGER NOT NULL REFERENCES monitored_roots(id) ON DELETE CASCADE,
            started_at INTEGER NOT NULL,
            completed_at INTEGER,
            total_allocated_bytes INTEGER NOT NULL DEFAULT 0,
            total_apparent_bytes INTEGER NOT NULL DEFAULT 0,
            total_inodes INTEGER NOT NULL DEFAULT 0,
            status TEXT NOT NULL DEFAULT 'IN_PROGRESS'
        );
        CREATE INDEX IF NOT EXISTS idx_snapshots_root_time ON snapshots(root_id, started_at);

        CREATE TABLE IF NOT EXISTS paths (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            parent_id INTEGER REFERENCES paths(id) ON DELETE CASCADE,
            name TEXT NOT NULL,
            full_path TEXT UNIQUE NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_paths_parent ON paths(parent_id);

        CREATE TABLE IF NOT EXISTS dir_metrics (
            snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
            path_id INTEGER NOT NULL REFERENCES paths(id) ON DELETE CASCADE,
            allocated_size INTEGER NOT NULL,
            apparent_size INTEGER NOT NULL,
            direct_files INTEGER NOT NULL,
            total_subdirs INTEGER NOT NULL,
            PRIMARY KEY (snapshot_id, path_id)
        ) WITHOUT ROWID;

        CREATE INDEX IF NOT EXISTS idx_dir_metrics_path ON dir_metrics(path_id, snapshot_id);
    )";

    return execute_sql(schema);
}

int64_t StorageEngine::get_or_create_root(const std::string &path) {
    std::string sql_find = "SELECT id FROM monitored_roots WHERE path = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_find.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return id;
        }
        sqlite3_finalize(stmt);
    }

    std::string sql_insert = "INSERT INTO monitored_roots (path, enabled, created_at) VALUES (?, 1, ?);";
    if (sqlite3_prepare_v2(db_, sql_insert.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, utils::current_timestamp());
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int64_t id = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(stmt);
            return id;
        }
        sqlite3_finalize(stmt);
    }
    return 0;
}

std::vector<RootRecord> StorageEngine::get_monitored_roots() {
    std::vector<RootRecord> roots;
    std::string sql = "SELECT id, path, enabled, created_at FROM monitored_roots ORDER BY id ASC;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RootRecord r;
            r.id = sqlite3_column_int64(stmt, 0);
            r.path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            r.enabled = sqlite3_column_int(stmt, 2) != 0;
            r.created_at = sqlite3_column_int64(stmt, 3);
            roots.push_back(r);
        }
        sqlite3_finalize(stmt);
    }
    return roots;
}

bool StorageEngine::set_root_enabled(const std::string &path, bool enabled) {
    std::string sql = "UPDATE monitored_roots SET enabled = ? WHERE path = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
        sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_STATIC);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
    return false;
}

bool StorageEngine::remove_root(const std::string &path) {
    std::string sql = "DELETE FROM monitored_roots WHERE path = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_STATIC);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
    return false;
}

int64_t StorageEngine::get_or_create_path(const std::string &full_path, int64_t parent_id, const std::string &name) {
    sqlite3_stmt *stmt = nullptr;
    std::string sql_find = "SELECT id FROM paths WHERE full_path = ?;";
    if (sqlite3_prepare_v2(db_, sql_find.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, full_path.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return id;
        }
        sqlite3_finalize(stmt);
    }

    std::string entry_name = name;
    if (entry_name.empty()) {
        fs::path p(full_path);
        entry_name = p.filename().string();
        if (entry_name.empty())
            entry_name = full_path;
    }

    std::string sql_insert = "INSERT INTO paths (parent_id, name, full_path) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql_insert.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (parent_id > 0) {
            sqlite3_bind_int64(stmt, 1, parent_id);
        } else {
            sqlite3_bind_null(stmt, 1);
        }
        sqlite3_bind_text(stmt, 2, entry_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, full_path.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int64_t id = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(stmt);
            return id;
        }
        sqlite3_finalize(stmt);
    }
    return 0;
}

std::optional<PathRecord> StorageEngine::get_path(int64_t path_id) {
    std::string sql = "SELECT id, parent_id, name, full_path FROM paths WHERE id = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, path_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            PathRecord r;
            r.id = sqlite3_column_int64(stmt, 0);
            r.parent_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 1);
            r.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            r.full_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            sqlite3_finalize(stmt);
            return r;
        }
        sqlite3_finalize(stmt);
    }
    return std::nullopt;
}

std::optional<PathRecord> StorageEngine::find_path_by_string(const std::string &full_path) {
    std::string sql = "SELECT id, parent_id, name, full_path FROM paths WHERE full_path = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, full_path.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            PathRecord r;
            r.id = sqlite3_column_int64(stmt, 0);
            r.parent_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 1);
            r.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            r.full_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            sqlite3_finalize(stmt);
            return r;
        }
        sqlite3_finalize(stmt);
    }
    return std::nullopt;
}

int64_t StorageEngine::create_snapshot(int64_t root_id) {
    std::string sql = "INSERT INTO snapshots (root_id, started_at, status) VALUES (?, ?, 'IN_PROGRESS');";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, root_id);
        sqlite3_bind_int64(stmt, 2, utils::current_timestamp());
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int64_t id = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(stmt);
            return id;
        }
        sqlite3_finalize(stmt);
    }
    return 0;
}

bool StorageEngine::finish_snapshot(int64_t snapshot_id, int64_t total_allocated, int64_t total_apparent,
                                    int64_t inodes) {
    std::string sql = "UPDATE snapshots SET completed_at = ?, total_allocated_bytes = ?, total_apparent_bytes = ?, "
                      "total_inodes = ?, status = 'COMPLETED' WHERE id = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, utils::current_timestamp());
        sqlite3_bind_int64(stmt, 2, total_allocated);
        sqlite3_bind_int64(stmt, 3, total_apparent);
        sqlite3_bind_int64(stmt, 4, inodes);
        sqlite3_bind_int64(stmt, 5, snapshot_id);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
    return false;
}

bool StorageEngine::abort_snapshot(int64_t snapshot_id) {
    std::string sql = "UPDATE snapshots SET completed_at = ?, status = 'ABORTED' WHERE id = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, utils::current_timestamp());
        sqlite3_bind_int64(stmt, 2, snapshot_id);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
    return false;
}

std::optional<SnapshotRecord> StorageEngine::get_latest_snapshot(int64_t root_id) {
    std::string sql = "SELECT id, root_id, started_at, completed_at, total_allocated_bytes, total_apparent_bytes, "
                      "total_inodes, status "
                      "FROM snapshots WHERE root_id = ? AND status = 'COMPLETED' ORDER BY started_at DESC LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, root_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            SnapshotRecord s;
            s.id = sqlite3_column_int64(stmt, 0);
            s.root_id = sqlite3_column_int64(stmt, 1);
            s.started_at = sqlite3_column_int64(stmt, 2);
            s.completed_at = sqlite3_column_int64(stmt, 3);
            s.total_allocated_bytes = sqlite3_column_int64(stmt, 4);
            s.total_apparent_bytes = sqlite3_column_int64(stmt, 5);
            s.total_inodes = sqlite3_column_int64(stmt, 6);
            s.status = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
            sqlite3_finalize(stmt);
            return s;
        }
        sqlite3_finalize(stmt);
    }
    return std::nullopt;
}

std::optional<SnapshotRecord> StorageEngine::get_snapshot_near_timestamp(int64_t root_id, int64_t target_timestamp) {
    // Find closest snapshot <= target_timestamp (or closest overall)
    std::string sql = "SELECT id, root_id, started_at, completed_at, total_allocated_bytes, total_apparent_bytes, "
                      "total_inodes, status "
                      "FROM snapshots WHERE root_id = ? AND status = 'COMPLETED' "
                      "ORDER BY ABS(started_at - ?) ASC LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, root_id);
        sqlite3_bind_int64(stmt, 2, target_timestamp);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            SnapshotRecord s;
            s.id = sqlite3_column_int64(stmt, 0);
            s.root_id = sqlite3_column_int64(stmt, 1);
            s.started_at = sqlite3_column_int64(stmt, 2);
            s.completed_at = sqlite3_column_int64(stmt, 3);
            s.total_allocated_bytes = sqlite3_column_int64(stmt, 4);
            s.total_apparent_bytes = sqlite3_column_int64(stmt, 5);
            s.total_inodes = sqlite3_column_int64(stmt, 6);
            s.status = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
            sqlite3_finalize(stmt);
            return s;
        }
        sqlite3_finalize(stmt);
    }
    return std::nullopt;
}

std::vector<SnapshotRecord> StorageEngine::get_recent_snapshots(int64_t root_id, size_t limit) {
    std::vector<SnapshotRecord> list;
    std::string sql = "SELECT id, root_id, started_at, completed_at, total_allocated_bytes, total_apparent_bytes, "
                      "total_inodes, status "
                      "FROM snapshots WHERE root_id = ? AND status = 'COMPLETED' ORDER BY started_at DESC LIMIT ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, root_id);
        sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(limit));
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SnapshotRecord s;
            s.id = sqlite3_column_int64(stmt, 0);
            s.root_id = sqlite3_column_int64(stmt, 1);
            s.started_at = sqlite3_column_int64(stmt, 2);
            s.completed_at = sqlite3_column_int64(stmt, 3);
            s.total_allocated_bytes = sqlite3_column_int64(stmt, 4);
            s.total_apparent_bytes = sqlite3_column_int64(stmt, 5);
            s.total_inodes = sqlite3_column_int64(stmt, 6);
            s.status = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
            list.push_back(s);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

int64_t StorageEngine::get_snapshot_dir_count(int64_t snapshot_id) {
    std::string sql = "SELECT COUNT(*) FROM dir_metrics WHERE snapshot_id = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, snapshot_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t count = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return count;
        }
        sqlite3_finalize(stmt);
    }
    return 0;
}

bool StorageEngine::insert_metrics_batch(const std::vector<DirMetric> &metrics) {
    if (metrics.empty())
        return true;

    execute_sql("BEGIN TRANSACTION;");
    std::string sql = "INSERT OR REPLACE INTO dir_metrics "
                      "(snapshot_id, path_id, allocated_size, apparent_size, direct_files, total_subdirs) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        execute_sql("ROLLBACK;");
        return false;
    }

    for (const auto &m : metrics) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, m.snapshot_id);
        sqlite3_bind_int64(stmt, 2, m.path_id);
        sqlite3_bind_int64(stmt, 3, m.allocated_size);
        sqlite3_bind_int64(stmt, 4, m.apparent_size);
        sqlite3_bind_int64(stmt, 5, m.direct_files);
        sqlite3_bind_int64(stmt, 6, m.total_subdirs);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            execute_sql("ROLLBACK;");
            return false;
        }
    }

    sqlite3_finalize(stmt);
    return execute_sql("COMMIT;");
}

std::vector<GrowthEntry> StorageEngine::get_fastest_growing(int64_t /*root_id*/, int64_t earlier_snapshot_id,
                                                            int64_t later_snapshot_id, int64_t parent_path_id,
                                                            bool direct_children_only, size_t limit) {
    std::vector<GrowthEntry> results;

    std::ostringstream ss;
    ss << "SELECT "
       << "  p.id, p.name, p.full_path, "
       << "  m2.allocated_size, "
       << "  (m2.allocated_size - COALESCE(m1.allocated_size, 0)) AS delta_alloc, "
       << "  m2.apparent_size, "
       << "  (m2.apparent_size - COALESCE(m1.apparent_size, 0)) AS delta_app, "
       << "  s2.started_at, s1.started_at "
       << "FROM paths p "
       << "JOIN dir_metrics m2 ON m2.path_id = p.id AND m2.snapshot_id = ? "
       << "JOIN snapshots s2 ON s2.id = m2.snapshot_id "
       << "LEFT JOIN dir_metrics m1 ON m1.path_id = p.id AND m1.snapshot_id = ? "
       << "LEFT JOIN snapshots s1 ON s1.id = ? ";

    if (direct_children_only) {
        if (parent_path_id > 0) {
            ss << "WHERE p.parent_id = ? ";
        } else {
            ss << "WHERE (p.parent_id IS NULL OR p.parent_id = 0) ";
        }
    } else if (parent_path_id > 0) {
        // Under parent subtree
        ss << "WHERE (p.parent_id = ? OR p.full_path LIKE (SELECT full_path || '/%' FROM paths WHERE id = ?)) ";
    }

    ss << "ORDER BY delta_alloc DESC LIMIT ?;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, ss.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare query: " << sqlite3_errmsg(db_) << std::endl;
        return results;
    }

    int bind_idx = 1;
    sqlite3_bind_int64(stmt, bind_idx++, later_snapshot_id);
    sqlite3_bind_int64(stmt, bind_idx++, earlier_snapshot_id);
    sqlite3_bind_int64(stmt, bind_idx++, earlier_snapshot_id);

    if (direct_children_only && parent_path_id > 0) {
        sqlite3_bind_int64(stmt, bind_idx++, parent_path_id);
    } else if (parent_path_id > 0) {
        sqlite3_bind_int64(stmt, bind_idx++, parent_path_id);
        sqlite3_bind_int64(stmt, bind_idx++, parent_path_id);
    }
    sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GrowthEntry e;
        e.path_id = sqlite3_column_int64(stmt, 0);
        e.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        e.full_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        e.current_allocated = sqlite3_column_int64(stmt, 3);
        e.delta_allocated = sqlite3_column_int64(stmt, 4);
        e.current_apparent = sqlite3_column_int64(stmt, 5);
        e.delta_apparent = sqlite3_column_int64(stmt, 6);

        int64_t t2 = sqlite3_column_int64(stmt, 7);
        int64_t t1 = sqlite3_column_type(stmt, 8) == SQLITE_NULL ? t2 : sqlite3_column_int64(stmt, 8);
        int64_t dt = std::max<int64_t>(1, t2 - t1);

        e.bytes_per_hour = (static_cast<double>(e.delta_allocated) / static_cast<double>(dt)) * 3600.0;
        int64_t prev_alloc = e.current_allocated - e.delta_allocated;
        if (prev_alloc > 0) {
            e.growth_percent = (static_cast<double>(e.delta_allocated) / static_cast<double>(prev_alloc)) * 100.0;
        } else if (e.delta_allocated > 0) {
            e.growth_percent = 100.0;
        } else {
            e.growth_percent = 0.0;
        }

        results.push_back(e);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<GrowthEntry> StorageEngine::get_children_growth(int64_t root_id, int64_t earlier_snapshot_id,
                                                            int64_t later_snapshot_id, int64_t parent_path_id) {
    return get_fastest_growing(root_id, earlier_snapshot_id, later_snapshot_id, parent_path_id, true, 200);
}

size_t StorageEngine::prune_old_snapshots(int retention_days) {
    int64_t threshold_time = utils::current_timestamp() - (retention_days * 86400LL);
    std::string sql = "DELETE FROM snapshots WHERE started_at < ?;";
    sqlite3_stmt *stmt = nullptr;
    size_t deleted = 0;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, threshold_time);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            deleted = sqlite3_changes(db_);
        }
        sqlite3_finalize(stmt);
    }
    return deleted;
}

bool StorageEngine::clear_all_data() {
    execute_sql("BEGIN TRANSACTION;");
    execute_sql("DELETE FROM dir_metrics;");
    execute_sql("DELETE FROM snapshots;");
    execute_sql("DELETE FROM paths;");
    execute_sql("COMMIT;");
    execute_sql("VACUUM;");
    return true;
}

} // namespace grocycle::db
