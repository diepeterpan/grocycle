#include "common/config.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace grocycle {

bool Config::validate(std::string &error_msg) const {
    if (scan_interval_seconds < MIN_INTERVAL_SECONDS) {
        error_msg = "Scan interval must be at least 10 minutes (600 seconds).";
        return false;
    }
    if (scan_interval_seconds > MAX_INTERVAL_SECONDS) {
        error_msg = "Scan interval cannot exceed 30 days (2592000 seconds).";
        return false;
    }
    if (retention_days < MIN_RETENTION_DAYS || retention_days > MAX_RETENTION_DAYS) {
        error_msg = "Retention days must be between 1 and 365.";
        return false;
    }
    if (monitored_roots.empty()) {
        error_msg = "At least one monitored root directory must be specified.";
        return false;
    }
    return true;
}

std::string Config::default_user_config_path() {
    const char *home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/grocycle/config.json";
    }
    return "/etc/grocycle/config.json";
}

Config Config::load_from_file(const std::string &path) {
    Config cfg;
    if (!fs::exists(path)) {
        // Fallback for non-root users: default paths in user directory
        if (getuid() != 0) {
            const char *home = std::getenv("HOME");
            if (home) {
                std::string h(home);
                cfg.db_path = h + "/.local/share/grocycle/grocycle.db";
                cfg.socket_path = h + "/.cache/grocycle/grocycle.sock";
                cfg.log_file = h + "/.cache/grocycle/grocycle.log";
            }
        }
        return cfg;
    }

    try {
        std::ifstream file(path);
        if (file.is_open()) {
            json j;
            file >> j;
            if (j.contains("db_path"))
                cfg.db_path = j["db_path"];
            if (j.contains("socket_path"))
                cfg.socket_path = j["socket_path"];
            if (j.contains("log_file"))
                cfg.log_file = j["log_file"];
            if (j.contains("scan_interval_seconds"))
                cfg.scan_interval_seconds = j["scan_interval_seconds"];
            if (j.contains("retention_days"))
                cfg.retention_days = j["retention_days"];
            if (j.contains("sleep_every_n_dirs"))
                cfg.sleep_every_n_dirs = j["sleep_every_n_dirs"];
            if (j.contains("sleep_duration_ms"))
                cfg.sleep_duration_ms = j["sleep_duration_ms"];
            if (j.contains("monitored_roots") && j["monitored_roots"].is_array()) {
                cfg.monitored_roots.clear();
                for (const auto &item : j["monitored_roots"]) {
                    cfg.monitored_roots.push_back(item);
                }
            }
            if (j.contains("excluded_prefixes") && j["excluded_prefixes"].is_array()) {
                cfg.excluded_prefixes.clear();
                for (const auto &item : j["excluded_prefixes"]) {
                    cfg.excluded_prefixes.push_back(item);
                }
            }
        }
    } catch (...) {
        // Fall back to default on parse errors
    }
    return cfg;
}

bool Config::save_to_file(const std::string &path) const {
    try {
        fs::path p(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        json j;
        j["db_path"] = db_path;
        j["socket_path"] = socket_path;
        j["log_file"] = log_file;
        j["scan_interval_seconds"] = scan_interval_seconds;
        j["retention_days"] = retention_days;
        j["sleep_every_n_dirs"] = sleep_every_n_dirs;
        j["sleep_duration_ms"] = sleep_duration_ms;
        j["monitored_roots"] = monitored_roots;
        j["excluded_prefixes"] = excluded_prefixes;

        std::ofstream file(path);
        if (!file.is_open())
            return false;
        file << j.dump(4) << std::endl;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace grocycle
