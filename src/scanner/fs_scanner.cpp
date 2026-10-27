#include "scanner/fs_scanner.hpp"
#include "common/utils.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace grocycle::scanner {

FsScanner::FsScanner(db::StorageEngine &storage, uint32_t throttle_steps, uint32_t throttle_sleep_ms,
                     std::vector<std::string> excluded_prefixes)
    : storage_(storage), throttle_(throttle_steps, throttle_sleep_ms),
      excluded_prefixes_(std::move(excluded_prefixes)) {}

bool FsScanner::is_excluded(const std::string &path) const {
    for (const auto &prefix : excluded_prefixes_) {
        if (path == prefix || (path.rfind(prefix + "/", 0) == 0)) {
            return true;
        }
    }
    return false;
}

ScanProgress FsScanner::get_progress() const {
    return progress_;
}

void FsScanner::set_progress_callback(std::function<void(const ScanProgress &)> cb) {
    progress_cb_ = std::move(cb);
}

bool FsScanner::scan_root(int64_t root_id, const std::string &root_path, std::atomic<bool> &cancel_flag) {
    struct stat root_st;
    if (lstat(root_path.c_str(), &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        std::cerr << "Root path is not an accessible directory: " << root_path << std::endl;
        return false;
    }

    progress_.current_root = root_path;
    progress_.current_path = root_path;
    progress_.scan_started_at = utils::current_timestamp();
    progress_.directories_scanned = 0;
    progress_.files_scanned = 0;
    progress_.total_allocated_bytes = 0;
    progress_.total_apparent_bytes = 0;
    progress_.is_running = true;

    int64_t snapshot_id = storage_.create_snapshot(root_id);
    if (snapshot_id <= 0) {
        progress_.is_running = false;
        return false;
    }

    std::vector<db::DirMetric> metrics;
    metrics.reserve(5000);

    DirStat total = scan_directory_recursive(snapshot_id, root_path, 0,
                                             root_st.st_dev, // Mount point boundary protection
                                             cancel_flag, metrics);

    if (cancel_flag.load()) {
        storage_.abort_snapshot(snapshot_id);
        progress_.is_running = false;
        return false;
    }

    // Flush any remaining metrics
    if (!metrics.empty()) {
        storage_.insert_metrics_batch(metrics);
        metrics.clear();
    }

    storage_.finish_snapshot(snapshot_id, total.total_allocated, total.total_apparent,
                             progress_.files_scanned + progress_.directories_scanned);

    progress_.is_running = false;
    return true;
}

FsScanner::DirStat FsScanner::scan_directory_recursive(int64_t snapshot_id, const std::string &current_dir,
                                                       int64_t parent_path_id, uint64_t expected_dev,
                                                       std::atomic<bool> &cancel_flag,
                                                       std::vector<db::DirMetric> &metrics_out) {
    DirStat dir_stat;
    if (cancel_flag.load())
        return dir_stat;

    if (is_excluded(current_dir)) {
        return dir_stat;
    }

    // Apply staggered sleep throttling
    throttle_.tick();

    // Register path in DB dictionary
    dir_stat.path_id = storage_.get_or_create_path(current_dir, parent_path_id);
    progress_.directories_scanned++;
    progress_.current_path = current_dir;

    if (progress_cb_ && (progress_.directories_scanned % 100 == 0)) {
        progress_cb_(progress_);
    }

    // Open directory with O_DIRECTORY and O_NOATIME (if supported)
    int fd = open(current_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC
#ifdef O_NOATIME
                                           | O_NOATIME
#endif
    );

    if (fd < 0) {
        // Fallback without O_NOATIME if permission denied (e.g. non-owner)
        fd = open(current_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return dir_stat;
        }
    }

    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return dir_stat;
    }

    std::vector<std::string> subdirectories;
    struct dirent *entry = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string entry_name = entry->d_name;
        std::string child_path = (current_dir == "/") ? ("/" + entry_name) : (current_dir + "/" + entry_name);

        struct stat st;
        // Use fstatat with AT_SYMLINK_NOFOLLOW and AT_NO_AUTOMOUNT for efficiency & safety
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }

        // Mount point boundary check: don't cross into other filesystems unless explicitly whitelisted
        if (st.st_dev != expected_dev) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            dir_stat.subdirs++;
            subdirectories.push_back(child_path);
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            dir_stat.direct_files++;
            progress_.files_scanned++;
            int64_t allocated = static_cast<int64_t>(st.st_blocks) * 512;
            int64_t apparent = static_cast<int64_t>(st.st_size);

            dir_stat.self_allocated += allocated;
            dir_stat.self_apparent += apparent;
            progress_.total_allocated_bytes += allocated;
            progress_.total_apparent_bytes += apparent;
        }
    }

    closedir(dir); // Also closes fd

    dir_stat.total_allocated = dir_stat.self_allocated;
    dir_stat.total_apparent = dir_stat.self_apparent;

    // Recurse into subdirectories
    for (const auto &subdir : subdirectories) {
        if (cancel_flag.load())
            break;
        DirStat child_stat =
            scan_directory_recursive(snapshot_id, subdir, dir_stat.path_id, expected_dev, cancel_flag, metrics_out);
        dir_stat.total_allocated += child_stat.total_allocated;
        dir_stat.total_apparent += child_stat.total_apparent;
    }

    // Add metric record for this directory
    db::DirMetric metric;
    metric.snapshot_id = snapshot_id;
    metric.path_id = dir_stat.path_id;
    metric.allocated_size = dir_stat.total_allocated;
    metric.apparent_size = dir_stat.total_apparent;
    metric.direct_files = dir_stat.direct_files;
    metric.total_subdirs = dir_stat.subdirs;
    metrics_out.push_back(metric);

    // Flush batch if exceeds buffer
    if (metrics_out.size() >= 5000) {
        storage_.insert_metrics_batch(metrics_out);
        metrics_out.clear();
    }

    return dir_stat;
}

} // namespace grocycle::scanner
