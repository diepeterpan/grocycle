#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace grocycle {

struct Config {
    std::string db_path = "/var/lib/grocycle/grocycle.db";
    std::string socket_path = "/run/grocycle/grocycle.sock";
    std::string log_file = "/var/log/grocycle.log";

    // Interval between scans (seconds). Range: 10 mins (600s) to 30 days (2592000s)
    uint32_t scan_interval_seconds = 3600; // 1 hour default

    // Low-impact throttling
    uint32_t sleep_every_n_dirs = 250;
    uint32_t sleep_duration_ms = 15;

    // Monitored roots (mount points or directories)
    std::vector<std::string> monitored_roots = {"/"};

    // Excluded path prefixes (pseudo-filesystems, etc.)
    std::vector<std::string> excluded_prefixes = {"/proc", "/sys", "/dev", "/run", "/tmp", "/var/tmp"};

    // Limits
    static constexpr uint32_t MIN_INTERVAL_SECONDS = 600;     // 10 minutes
    static constexpr uint32_t MAX_INTERVAL_SECONDS = 2592000; // 30 days

    bool validate(std::string &error_msg) const;
    static Config load_from_file(const std::string &path);
    bool save_to_file(const std::string &path) const;
    static std::string default_user_config_path();
};

} // namespace grocycle
