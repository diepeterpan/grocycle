#pragma once

#include "db/storage_engine.hpp"
#include "scanner/throttle.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>

namespace grocycle::scanner {

struct ScanProgress {
    std::string current_root;
    std::string current_path;
    int64_t scan_started_at = 0;
    int64_t directories_scanned = 0;
    int64_t files_scanned = 0;
    int64_t total_allocated_bytes = 0;
    int64_t total_apparent_bytes = 0;
    bool is_running = false;
};

class FsScanner {
public:
    FsScanner(db::StorageEngine &storage, uint32_t throttle_steps = 250, uint32_t throttle_sleep_ms = 15,
              std::vector<std::string> excluded_prefixes = {});

    // Scans a specific root directory and records snapshot in storage
    bool scan_root(int64_t root_id, const std::string &root_path, std::atomic<bool> &cancel_flag);

    // Get live progress
    ScanProgress get_progress() const;

    // Callback on scan progress update
    void set_progress_callback(std::function<void(const ScanProgress &)> cb);

private:
    struct DirStat {
        int64_t path_id = 0;
        int64_t self_allocated = 0; // files directly in dir
        int64_t self_apparent = 0;
        int64_t direct_files = 0;
        int64_t subdirs = 0;
        int64_t total_allocated = 0; // self + recursive children
        int64_t total_apparent = 0;
    };

    db::StorageEngine &storage_;
    Throttle throttle_;
    std::vector<std::string> excluded_prefixes_;
    ScanProgress progress_;
    mutable std::mutex progress_mutex_;
    std::function<void(const ScanProgress &)> progress_cb_;

    bool is_excluded(const std::string &path) const;
    DirStat scan_directory_recursive(int64_t snapshot_id, const std::string &current_dir, int64_t parent_path_id,
                                     uint64_t expected_dev, std::atomic<bool> &cancel_flag,
                                     std::vector<db::DirMetric> &metrics_out);
};

} // namespace grocycle::scanner
