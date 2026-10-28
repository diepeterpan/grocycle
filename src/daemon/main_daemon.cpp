#include "common/config.hpp"
#include "common/utils.hpp"
#include "db/storage_engine.hpp"
#include "scanner/fs_scanner.hpp"
#include "scanner/throttle.hpp"
#include "ipc/unix_socket.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <csignal>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

using json = nlohmann::json;

namespace {
std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_scan_now_requested{false};
std::atomic<bool> g_is_scanning{false};

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop_requested = true;
    }
}
} // namespace

int main(int argc, char **argv) {
    CLI::App app{"grocycled: Background filesystem growth measurement daemon"};

    std::string config_path = "";
    bool foreground = false;

    app.add_option("-c,--config", config_path, "Path to configuration file");
    app.add_flag("-f,--foreground", foreground, "Run in foreground (do not daemonize)");

    CLI11_PARSE(app, argc, argv);

    if (config_path.empty()) {
        config_path = grocycle::Config::default_user_config_path();
    }

    grocycle::Config cfg = grocycle::Config::load_from_file(config_path);
    std::string err;
    if (!cfg.validate(err)) {
        std::cerr << "Configuration error: " << err << std::endl;
        return 1;
    }

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Daemonize if not running in foreground
    if (!foreground) {
        if (daemon(0, 0) != 0) {
            std::cerr << "Failed to daemonize process" << std::endl;
            return 1;
        }
    }

    // Apply low I/O & CPU priority
    grocycle::scanner::Throttle::apply_low_priority_scheduling();

    // Initialize database
    grocycle::db::StorageEngine storage(cfg.db_path);
    if (!storage.initialize()) {
        std::cerr << "Failed to initialize storage engine at: " << cfg.db_path << std::endl;
        return 1;
    }

    // Ensure configured roots exist in DB
    for (const auto &root_str : cfg.monitored_roots) {
        storage.get_or_create_root(root_str);
    }

    // Setup FsScanner
    grocycle::scanner::FsScanner scanner(storage, cfg.sleep_every_n_dirs, cfg.sleep_duration_ms, cfg.excluded_prefixes);

    int64_t last_scan_time = 0;
    auto startup_roots = storage.get_monitored_roots();
    if (!startup_roots.empty()) {
        auto latest_snap = storage.get_latest_snapshot(startup_roots.front().id);
        if (latest_snap.has_value()) {
            last_scan_time = latest_snap->completed_at > 0 ? latest_snap->completed_at : latest_snap->started_at;
        }
    }
    int64_t next_scan_time =
        (last_scan_time > 0) ? (last_scan_time + cfg.scan_interval_seconds) : grocycle::utils::current_timestamp();

    // Setup IPC Socket Server
    grocycle::ipc::SocketServer server(cfg.socket_path, [&](const json &req) -> json {
        std::string cmd = req.value("command", "");
        if (cmd == "ping") {
            return {{"status", "ok"}, {"message", "pong"}, {"pid", getpid()}};
        } else if (cmd == "status") {
            auto progress = scanner.get_progress();
            int64_t last_scan_total_dirs = 0;
            auto roots = storage.get_monitored_roots();
            if (!roots.empty()) {
                auto latest_snap = storage.get_latest_snapshot(roots.front().id);
                if (latest_snap.has_value()) {
                    last_scan_total_dirs = storage.get_snapshot_dir_count(latest_snap->id);
                }
            }
            return {{"status", "ok"},
                    {"pid", getpid()},
                    {"is_scanning", g_is_scanning.load()},
                    {"scan_interval_seconds", cfg.scan_interval_seconds},
                    {"retention_days", cfg.retention_days},
                    {"last_scan_time", last_scan_time},
                    {"next_scan_time", next_scan_time},
                    {"monitored_roots", cfg.monitored_roots},
                    {"progress",
                     {{"current_root", progress.current_root},
                      {"current_path", progress.current_path},
                      {"scan_started_at", progress.scan_started_at},
                      {"dirs_scanned", progress.directories_scanned},
                      {"files_scanned", progress.files_scanned},
                      {"allocated_bytes", progress.total_allocated_bytes},
                      {"apparent_bytes", progress.total_apparent_bytes},
                      {"last_scan_total_dirs", last_scan_total_dirs}}}};
        } else if (cmd == "scan_now") {
            if (g_is_scanning.load()) {
                return {{"status", "error"}, {"message", "Scan already in progress"}};
            }
            g_scan_now_requested = true;
            return {{"status", "ok"}, {"message", "Scan triggered"}};
        } else if (cmd == "set_interval") {
            uint32_t new_interval = req.value("interval_seconds", 0);
            if (new_interval < grocycle::Config::MIN_INTERVAL_SECONDS ||
                new_interval > grocycle::Config::MAX_INTERVAL_SECONDS) {
                return {{"status", "error"}, {"message", "Interval must be between 10m and 30d"}};
            }
            cfg.scan_interval_seconds = new_interval;
            cfg.save_to_file(config_path);
            next_scan_time = last_scan_time + new_interval;
            return {{"status", "ok"}, {"interval_seconds", cfg.scan_interval_seconds}};
        } else if (cmd == "set_retention") {
            uint32_t days = req.value("retention_days", 0);
            if (days < grocycle::Config::MIN_RETENTION_DAYS || days > grocycle::Config::MAX_RETENTION_DAYS) {
                return {{"status", "error"}, {"message", "Retention must be between 1 and 365 days"}};
            }
            cfg.retention_days = days;
            cfg.save_to_file(config_path);
            storage.prune_old_snapshots(cfg.retention_days);
            return {{"status", "ok"}, {"retention_days", cfg.retention_days}};
        } else if (cmd == "add_root") {
            std::string root = req.value("path", "");
            if (root.empty())
                return {{"status", "error"}, {"message", "Path required"}};
            storage.get_or_create_root(root);
            if (std::find(cfg.monitored_roots.begin(), cfg.monitored_roots.end(), root) == cfg.monitored_roots.end()) {
                cfg.monitored_roots.push_back(root);
                cfg.save_to_file(config_path);
            }
            return {{"status", "ok"}, {"monitored_roots", cfg.monitored_roots}};
        } else if (cmd == "remove_root") {
            std::string root = req.value("path", "");
            if (root.empty())
                return {{"status", "error"}, {"message", "Path required"}};
            storage.remove_root(root);
            auto it = std::find(cfg.monitored_roots.begin(), cfg.monitored_roots.end(), root);
            if (it != cfg.monitored_roots.end()) {
                cfg.monitored_roots.erase(it);
                cfg.save_to_file(config_path);
            }
            return {{"status", "ok"}, {"monitored_roots", cfg.monitored_roots}};
        } else if (cmd == "clean") {
            if (g_is_scanning.load()) {
                return {{"status", "error"}, {"message", "Cannot clean while a scan is in progress"}};
            }
            bool ok = storage.clear_all_data();
            if (ok) {
                // Ensure configured roots are re-registered
                for (const auto &root_str : cfg.monitored_roots) {
                    storage.get_or_create_root(root_str);
                }
                last_scan_time = 0;
                next_scan_time = grocycle::utils::current_timestamp(); // Schedule fresh scan immediately
                return {{"status", "ok"}, {"message", "Database successfully cleaned and reset."}};
            }
            return {{"status", "error"}, {"message", "Failed to clear database"}};
        } else if (cmd == "stop") {
            g_stop_requested = true;
            return {{"status", "ok"}, {"message", "Stopping daemon..."}};
        }
        return {{"status", "error"}, {"message", "Unknown command"}};
    });

    if (!server.start()) {
        std::cerr << "Failed to start IPC socket server at " << cfg.socket_path << std::endl;
        return 1;
    }

    std::cout << "grocycled started successfully. PID: " << getpid() << std::endl;

    // Main Service Loop
    while (!g_stop_requested.load()) {
        int64_t now = grocycle::utils::current_timestamp();
        bool due = (now >= next_scan_time) || g_scan_now_requested.load();

        if (due && !g_is_scanning.load()) {
            g_scan_now_requested = false;
            g_is_scanning = true;

            auto roots = storage.get_monitored_roots();
            for (const auto &r : roots) {
                if (g_stop_requested.load())
                    break;
                if (!r.enabled)
                    continue;

                std::atomic<bool> cancel_scan{false};
                scanner.scan_root(r.id, r.path, cancel_scan);
            }

            // Prune snapshots older than configured retention days
            storage.prune_old_snapshots(cfg.retention_days);

            last_scan_time = grocycle::utils::current_timestamp();
            next_scan_time = last_scan_time + cfg.scan_interval_seconds;
            g_is_scanning = false;
        }

        // Sleep in 1-second slices to remain responsive to stop/scan requests
        for (int i = 0; i < 2 && !g_stop_requested.load() && !g_scan_now_requested.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    server.stop();
    storage.close();
    std::cout << "grocycled stopped." << std::endl;
    return 0;
}
