#include "cli/cli_commands.hpp"
#include "common/utils.hpp"
#include "db/storage_engine.hpp"
#include "ipc/unix_socket.hpp"
#include "web/web_server.hpp"
#include <iostream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <unistd.h>

using json = nlohmann::json;

namespace grocycle::cli {

int run_top_command(const Config &cfg, const std::string &window_str, const std::string &path_str, size_t limit,
                    const std::string &format) {
    uint32_t window_sec = 3600; // default 1 hour
    if (!utils::parse_duration(window_str, window_sec)) {
        std::cerr << "Invalid time window duration: " << window_str << std::endl;
        return 1;
    }

    db::StorageEngine storage(cfg.db_path);
    if (!storage.initialize()) {
        std::cerr << "Cannot open database at: " << cfg.db_path << std::endl;
        return 1;
    }

    auto roots = storage.get_monitored_roots();
    if (roots.empty()) {
        std::cerr << "No monitored roots found. Start the daemon first or add a root." << std::endl;
        return 1;
    }

    int64_t target_root_id = roots[0].id;
    int64_t parent_path_id = 0;

    if (!path_str.empty()) {
        auto p_rec = storage.find_path_by_string(path_str);
        if (p_rec.has_value()) {
            parent_path_id = p_rec->id;
        }
    }

    auto latest_snap = storage.get_latest_snapshot(target_root_id);
    if (!latest_snap.has_value()) {
        std::cerr << "No completed snapshots found in database yet. Allow grocycled time to perform the initial scan."
                  << std::endl;
        return 1;
    }

    int64_t target_time = latest_snap->started_at - window_sec;
    auto earlier_snap = storage.get_snapshot_near_timestamp(target_root_id, target_time);
    int64_t earlier_snap_id = earlier_snap.has_value() ? earlier_snap->id : latest_snap->id;

    auto growth =
        storage.get_fastest_growing(target_root_id, earlier_snap_id, latest_snap->id, parent_path_id, false, limit);

    if (format == "json") {
        json j = json::array();
        for (const auto &g : growth) {
            j.push_back({{"path", g.full_path},
                         {"name", g.name},
                         {"current_allocated_bytes", g.current_allocated},
                         {"delta_allocated_bytes", g.delta_allocated},
                         {"current_apparent_bytes", g.current_apparent},
                         {"delta_apparent_bytes", g.delta_apparent},
                         {"growth_rate_bytes_per_hour", g.bytes_per_hour},
                         {"growth_percent", g.growth_percent}});
        }
        std::cout << j.dump(2) << std::endl;
        return 0;
    }

    // Default: Terminal table output
    std::cout << "\n=== Grocycle: Top Growing Directories (Window: " << window_str << ") ===" << std::endl;
    std::cout << "Comparing Snapshot #" << earlier_snap_id << " -> Snapshot #" << latest_snap->id << "\n" << std::endl;

    std::cout << std::left << std::setw(42) << "Directory Path" << std::right << std::setw(14) << "Current Size"
              << std::setw(15) << "Growth (Δ)" << std::setw(16) << "Growth Rate" << std::setw(12) << "Growth %"
              << std::endl;
    std::cout << std::string(99, '-') << std::endl;

    for (const auto &g : growth) {
        std::string disp_path = g.full_path;
        if (disp_path.length() > 40) {
            disp_path = "..." + disp_path.substr(disp_path.length() - 37);
        }

        std::cout << std::left << std::setw(42) << disp_path << std::right << std::setw(14)
                  << utils::format_bytes(g.current_allocated) << std::setw(15)
                  << utils::format_bytes(g.delta_allocated, true) << std::setw(16)
                  << (utils::format_bytes(static_cast<int64_t>(g.bytes_per_hour)) + "/h") << std::setw(11) << std::fixed
                  << std::setprecision(1) << g.growth_percent << "%" << std::endl;
    }
    std::cout << std::endl;
    return 0;
}

int run_service_command(const Config &cfg, const std::string &action, const std::string & /*extra_arg*/
) {
    ipc::SocketClient client(cfg.socket_path);

    if (action == "status") {
        if (!client.is_daemon_running()) {
            std::cout << "grocycled service: STOPPED (Socket not accessible: " << cfg.socket_path << ")" << std::endl;
            return 0;
        }
        json res = client.send_command({{"command", "status"}});
        if (res.value("status", "") == "ok") {
            std::cout << "grocycled service: RUNNING" << std::endl;
            std::cout << "  PID: " << res.value("pid", 0) << std::endl;
            std::cout << "  Scanning Now: " << (res.value("is_scanning", false) ? "YES" : "NO (Idle)") << std::endl;
            std::cout << "  Interval: " << utils::format_duration(res.value("scan_interval_seconds", 3600))
                      << std::endl;
            int64_t last_s = res.value("last_scan_time", 0);
            if (last_s > 0) {
                std::cout << "  Last Scan: " << utils::format_timestamp(last_s) << std::endl;
            }
            int64_t next_s = res.value("next_scan_time", 0);
            if (next_s > 0) {
                std::cout << "  Next Scan: " << utils::format_timestamp(next_s) << std::endl;
            }
            if (res.contains("progress") && res["is_scanning"].get<bool>()) {
                auto p = res["progress"];
                std::cout << "  Progress: " << p.value("dirs_scanned", 0) << " dirs, " << p.value("files_scanned", 0)
                          << " files (" << utils::format_bytes(p.value("allocated_bytes", 0)) << ")" << std::endl;
            }
            return 0;
        } else {
            std::cerr << "Failed to query status: " << res.value("message", "Unknown error") << std::endl;
            return 1;
        }
    } else if (action == "start") {
        if (client.is_daemon_running()) {
            std::cout << "grocycled is already running." << std::endl;
            return 0;
        }
        std::cout << "Starting grocycled in background..." << std::endl;
        std::string cmd = "grocycled >/dev/null 2>&1 &";
        int ret = system(cmd.c_str());
        usleep(300000); // 300ms
        if (client.is_daemon_running()) {
            std::cout << "grocycled started successfully." << std::endl;
            return 0;
        } else {
            std::cerr << "Failed to start grocycled. (exit code: " << ret << ")" << std::endl;
            return 1;
        }
    } else if (action == "stop") {
        if (!client.is_daemon_running()) {
            std::cout << "grocycled is not running." << std::endl;
            return 0;
        }
        json res = client.send_command({{"command", "stop"}});
        std::cout << res.value("message", "Stopping daemon...") << std::endl;
        return 0;
    } else if (action == "scan") {
        if (!client.is_daemon_running()) {
            std::cerr << "grocycled is not running. Start it first with 'grocycle service start'." << std::endl;
            return 1;
        }
        json res = client.send_command({{"command", "scan_now"}});
        std::cout << res.value("message", "") << std::endl;
        return 0;
    }

    std::cerr << "Unknown service action: " << action << ". Use status, start, stop, or scan." << std::endl;
    return 1;
}

int run_config_command(Config &cfg, const std::string &config_file, const std::string &action, const std::string &val1,
                       const std::string & /*val2*/
) {
    ipc::SocketClient client(cfg.socket_path);

    if (action == "set-interval") {
        uint32_t seconds = 0;
        if (!utils::parse_duration(val1, seconds)) {
            std::cerr << "Invalid duration format: " << val1 << " (example: 10m, 1h, 7d)" << std::endl;
            return 1;
        }
        if (seconds < Config::MIN_INTERVAL_SECONDS || seconds > Config::MAX_INTERVAL_SECONDS) {
            std::cerr << "Interval must be between 10m and 30d." << std::endl;
            return 1;
        }

        cfg.scan_interval_seconds = seconds;
        cfg.save_to_file(config_file);

        if (client.is_daemon_running()) {
            client.send_command({{"command", "set_interval"}, {"interval_seconds", seconds}});
        }
        std::cout << "Scan interval set to " << utils::format_duration(seconds) << "." << std::endl;
        return 0;
    } else if (action == "add-root") {
        if (val1.empty()) {
            std::cerr << "Specify path to add: grocycle config add-root <path>" << std::endl;
            return 1;
        }
        if (client.is_daemon_running()) {
            client.send_command({{"command", "add_root"}, {"path", val1}});
        } else {
            db::StorageEngine storage(cfg.db_path);
            if (storage.initialize()) {
                storage.get_or_create_root(val1);
            }
            cfg.monitored_roots.push_back(val1);
            cfg.save_to_file(config_file);
        }
        std::cout << "Added monitored root: " << val1 << std::endl;
        return 0;
    } else if (action == "remove-root") {
        if (val1.empty()) {
            std::cerr << "Specify path to remove: grocycle config remove-root <path>" << std::endl;
            return 1;
        }
        if (client.is_daemon_running()) {
            client.send_command({{"command", "remove_root"}, {"path", val1}});
        } else {
            db::StorageEngine storage(cfg.db_path);
            if (storage.initialize()) {
                storage.remove_root(val1);
            }
            auto it = std::find(cfg.monitored_roots.begin(), cfg.monitored_roots.end(), val1);
            if (it != cfg.monitored_roots.end()) {
                cfg.monitored_roots.erase(it);
                cfg.save_to_file(config_file);
            }
        }
        std::cout << "Removed monitored root: " << val1 << std::endl;
        return 0;
    } else if (action == "show") {
        std::cout << "Grocycle Configuration (" << config_file << "):" << std::endl;
        std::cout << "  Database: " << cfg.db_path << std::endl;
        std::cout << "  Socket:   " << cfg.socket_path << std::endl;
        std::cout << "  Interval: " << utils::format_duration(cfg.scan_interval_seconds) << std::endl;
        std::cout << "  Monitored roots:" << std::endl;
        for (const auto &r : cfg.monitored_roots) {
            std::cout << "    - " << r << std::endl;
        }
        return 0;
    }

    std::cerr << "Unknown config action: " << action << ". Use show, set-interval, add-root, or remove-root."
              << std::endl;
    return 1;
}

int run_clean_command(const Config &cfg, bool force) {
    if (!force) {
        std::cout << "Are you sure you want to reset and wipe the Grocycle database (" << cfg.db_path << ")? [y/N]: ";
        std::cout.flush();
        std::string reply;
        if (!std::getline(std::cin, reply) || (reply != "y" && reply != "Y")) {
            std::cout << "Clean aborted." << std::endl;
            return 0;
        }
    }

    ipc::SocketClient client(cfg.socket_path);
    if (client.is_daemon_running()) {
        std::cout << "Connecting to running grocycled daemon..." << std::endl;
        json res = client.send_command({{"command", "clean"}});
        if (res.value("status", "") == "ok") {
            std::cout << res.value("message", "Database successfully cleaned.") << std::endl;
            // Also trigger fresh scan
            client.send_command({{"command", "scan_now"}});
            std::cout << "Fresh scan initiated." << std::endl;
            return 0;
        } else {
            std::cerr << "Error: " << res.value("message", "Unknown error") << std::endl;
            return 1;
        }
    } else {
        // Direct database reset when daemon is stopped
        std::cout << "grocycled is not running. Resetting database directly..." << std::endl;
        db::StorageEngine storage(cfg.db_path);
        if (!storage.initialize()) {
            std::cerr << "Could not open database at: " << cfg.db_path << std::endl;
            return 1;
        }
        if (storage.clear_all_data()) {
            // Re-register roots
            for (const auto &r : cfg.monitored_roots) {
                storage.get_or_create_root(r);
            }
            std::cout << "Database successfully cleaned and reset." << std::endl;
            return 0;
        } else {
            std::cerr << "Failed to clean database." << std::endl;
            return 1;
        }
    }
}

int run_web_command(const Config &cfg, const std::string &config_file, uint16_t port, const std::string &host,
                    const std::string &auth, bool open_browser, size_t max_clients) {
    web::ServerOptions options;
    options.port = port;
    options.host = host;
    options.auth = auth;
    options.auto_open_browser = open_browser;
    options.max_clients = max_clients;
    options.config_path = config_file;

    return web::run_web_server(cfg, options);
}

} // namespace grocycle::cli
