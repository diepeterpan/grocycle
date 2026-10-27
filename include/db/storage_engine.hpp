#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>
#include <mutex>

struct sqlite3;

namespace grocycle::db {

struct RootRecord {
    int64_t id = 0;
    std::string path;
    bool enabled = true;
    int64_t created_at = 0;
};

struct SnapshotRecord {
    int64_t id = 0;
    int64_t root_id = 0;
    int64_t started_at = 0;
    int64_t completed_at = 0;
    int64_t total_allocated_bytes = 0;
    int64_t total_apparent_bytes = 0;
    int64_t total_inodes = 0;
    std::string status; // "IN_PROGRESS", "COMPLETED", "ABORTED"
};

struct PathRecord {
    int64_t id = 0;
    int64_t parent_id = 0;
    std::string name;
    std::string full_path;
};

struct DirMetric {
    int64_t snapshot_id = 0;
    int64_t path_id = 0;
    int64_t allocated_size = 0; // st_blocks * 512
    int64_t apparent_size = 0;  // st_size
    int64_t direct_files = 0;
    int64_t total_subdirs = 0;
};

struct GrowthEntry {
    int64_t path_id = 0;
    std::string name;
    std::string full_path;
    int64_t current_allocated = 0;
    int64_t delta_allocated = 0;
    int64_t current_apparent = 0;
    int64_t delta_apparent = 0;
    double bytes_per_hour = 0.0;
    double growth_percent = 0.0;
    bool is_directory = true;
};

class StorageEngine {
public:
    explicit StorageEngine(std::string db_path);
    ~StorageEngine();

    // Prevent copy
    StorageEngine(const StorageEngine &) = delete;
    StorageEngine &operator=(const StorageEngine &) = delete;

    bool initialize();
    void close();

    // Roots
    int64_t get_or_create_root(const std::string &path);
    std::vector<RootRecord> get_monitored_roots();
    bool set_root_enabled(const std::string &path, bool enabled);
    bool remove_root(const std::string &path);

    // Paths
    int64_t get_or_create_path(const std::string &full_path, int64_t parent_id = 0, const std::string &name = "");
    std::optional<PathRecord> get_path(int64_t path_id);
    std::optional<PathRecord> find_path_by_string(const std::string &full_path);

    // Snapshots
    int64_t create_snapshot(int64_t root_id);
    bool finish_snapshot(int64_t snapshot_id, int64_t total_allocated, int64_t total_apparent, int64_t inodes);
    bool abort_snapshot(int64_t snapshot_id);
    std::optional<SnapshotRecord> get_latest_snapshot(int64_t root_id);
    std::optional<SnapshotRecord> get_snapshot_near_timestamp(int64_t root_id, int64_t target_timestamp);
    std::vector<SnapshotRecord> get_recent_snapshots(int64_t root_id, size_t limit = 10);
    int64_t get_snapshot_dir_count(int64_t snapshot_id);

    // Metrics batch insertion
    bool insert_metrics_batch(const std::vector<DirMetric> &metrics);

    // Analytical Queries
    // Get top fastest growing directories under a given path (or entire root) across two snapshots
    std::vector<GrowthEntry> get_fastest_growing(int64_t root_id, int64_t earlier_snapshot_id,
                                                 int64_t later_snapshot_id,
                                                 int64_t parent_path_id = 0, // 0 = all subtrees or direct children
                                                 bool direct_children_only = false, size_t limit = 50);

    // Query immediate children of a path with growth calculation for tree drill-down
    std::vector<GrowthEntry> get_children_growth(int64_t root_id, int64_t earlier_snapshot_id,
                                                 int64_t later_snapshot_id, int64_t parent_path_id);

    // Maintenance: Prune snapshots older than retention days (default 30 days)
    size_t prune_old_snapshots(int retention_days = 30);

    // Completely wipe historical snapshots, metrics, and paths
    bool clear_all_data();

private:
    std::string db_path_;
    sqlite3 *db_ = nullptr;
    mutable std::recursive_mutex db_mutex_;

    bool execute_sql(const std::string &sql);
};

} // namespace grocycle::db
