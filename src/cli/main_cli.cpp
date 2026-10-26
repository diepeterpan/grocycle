#include "common/config.hpp"
#include "common/version.hpp"
#include "cli/cli_commands.hpp"
#include "tui/tui_app.hpp"
#include <CLI/CLI.hpp>
#include <iostream>

int main(int argc, char **argv) {
    CLI::App app{"Grocycle: Native Linux Directory Growth Explorer & Controller\n"
                 "Measures and identifies the fastest growing directories over configurable time windows.\n"
                 "Run without subcommands to launch the interactive TUI."};

    app.set_version_flag("-v,--version", std::string("grocycle ") + grocycle::version::FULL_VERSION_STRING);

    std::string config_path = "";
    app.add_option(
        "-c,--config", config_path,
        "Path to JSON configuration file (default: ~/.config/grocycle/config.json or /etc/grocycle/config.json)");

    // 1. top
    auto top_sub = app.add_subcommand("top", "Show fastest growing directories in terminal (table or JSON)\n"
                                             "  Examples:\n"
                                             "    grocycle top\n"
                                             "    grocycle top -w 24h -l 10\n"
                                             "    grocycle top -w 1h -p /var -f json");
    std::string window = "1h";
    std::string path_filter = "";
    size_t limit = 15;
    std::string format = "table";
    top_sub->add_option("-w,--window", window, "Time horizon (e.g. 10m, 1h, 6h, 24h, 7d, 30d). Valid range: 10m to 30d")
        ->default_str("1h");
    top_sub->add_option("-p,--path", path_filter, "Subdirectory path to filter query to (e.g. /var, /home)");
    top_sub->add_option("-l,--limit", limit, "Number of directory results to return (range: 1-1000)")->default_val(15);
    top_sub->add_option("-f,--format", format, "Output format: table (human-readable) or json (machine-readable)")
        ->check(CLI::IsMember({"table", "json"}))
        ->default_str("table");

    // 2. service
    auto service_sub = app.add_subcommand(
        "service", "Manage the background monitoring daemon (grocycled)\n"
                   "  Actions:\n"
                   "    status  - Show daemon status, PID, last/next scan times, and live scan progress\n"
                   "    start   - Start grocycled background daemon\n"
                   "    stop    - Gracefully stop grocycled\n"
                   "    scan    - Trigger an immediate out-of-schedule scan");
    std::string service_action = "status";
    service_sub->add_option("action", service_action, "Service action to perform: status | start | stop | scan")
        ->check(CLI::IsMember({"status", "start", "stop", "scan"}))
        ->default_str("status");

    // 3. config
    auto config_sub = app.add_subcommand(
        "config", "View and modify daemon configuration\n"
                  "  Actions:\n"
                  "    show                 - Print active configuration and monitored paths\n"
                  "    set-interval <time>  - Update scan interval. Valid range: 10m (600s) to 30d (2592000s)\n"
                  "                           Units supported: m, min, h, hr, d, day (e.g., 15m, 2h, 7d)\n"
                  "    add-root <path>      - Add a target directory or mount point to monitor (e.g., /data)\n"
                  "    remove-root <path>   - Stop monitoring a directory path");
    std::string config_action = "show";
    std::string val1 = "";
    std::string val2 = "";
    config_sub->add_option("action", config_action, "Config action: show | set-interval | add-root | remove-root")
        ->check(CLI::IsMember({"show", "set-interval", "add-root", "remove-root"}))
        ->default_str("show");
    config_sub->add_option("arg1", val1, "Value for action: interval duration (10m - 30d) or directory path");
    config_sub->add_option("arg2", val2, "Optional second argument");

    // 4. clean
    auto clean_sub = app.add_subcommand(
        "clean", "Reset and wipe historical snapshots and metric entries from the database\n"
                 "  Works whether grocycled is running or stopped. Reclaims disk space and restarts baseline scan.");
    bool force_clean = false;
    clean_sub->add_flag("-y,--yes,-f,--force", force_clean, "Execute clean without interactive confirmation prompt");

    // 5. tui
    auto tui_sub = app.add_subcommand(
        "tui",
        "Launch the interactive full-color terminal drill-down explorer (default when no subcommand is specified)");

    // 6. web
    auto web_sub = app.add_subcommand(
        "web", "Start the embedded web server and launch Grocycle interactive terminal UI in a web browser\n"
               "  Examples:\n"
               "    grocycle web\n"
               "    grocycle web --port 8080\n"
               "    grocycle web --port 9000 --host 0.0.0.0\n"
               "    grocycle web --auth admin:secret123 --browser");
    uint16_t web_port = 8080;
    std::string web_host = "127.0.0.1";
    std::string web_auth = "";
    bool open_browser = false;
    size_t max_clients = 5;

    web_sub->add_option("-p,--port", web_port, "TCP port to listen on (range: 1024-65535)")
        ->default_val(8080)
        ->check(CLI::Range(1024, 65535));
    web_sub->add_option("-H,--host", web_host, "Interface host/IP to bind to (default: 127.0.0.1)")
        ->default_str("127.0.0.1");
    web_sub->add_option("-a,--auth", web_auth, "Optional HTTP Basic Auth in 'username:password' format");
    web_sub->add_flag("-b,--browser", open_browser, "Automatically launch default system browser");
    web_sub->add_option("--max-clients", max_clients, "Maximum concurrent web terminal sessions (range: 1-50)")
        ->default_val(5)
        ->check(CLI::Range(1, 50));

    // Provide detailed footer in main help listing all subcommands and parameters
    app.footer("Subcommands & Value Ranges Reference:\n"
               "  grocycle top\n"
               "    -w, --window TEXT    Time window: 10m, 1h, 6h, 24h, 7d, 30d (Range: 10m - 30d) [default: 1h]\n"
               "    -p, --path TEXT      Subdirectory path filter (e.g. /var, /home/user)\n"
               "    -l, --limit INT      Max results to display [default: 15]\n"
               "    -f, --format TEXT    Output format: 'table' or 'json' [default: table]\n\n"
               "  grocycle service <action>\n"
               "    action               'status' (default) | 'start' | 'stop' | 'scan'\n\n"
               "  grocycle config <action> [value]\n"
               "    show                 Display active configuration\n"
               "    set-interval <time>  Set scan frequency: 10m <= interval <= 30d (e.g. 15m, 1h, 14d)\n"
               "    add-root <path>      Add directory/mount point to monitoring list\n"
               "    remove-root <path>   Remove directory from monitoring list\n\n"
               "  grocycle clean\n"
               "    -y, --yes, -f        Skip confirmation prompt and reset database immediately\n\n"
               "  grocycle tui\n"
               "    Launch interactive drill-down explorer with arrow/vim keys and color indicators\n\n"
               "  grocycle web [options]\n"
               "    -p, --port INT       TCP port to listen on (range: 1024-65535) [default: 8080]\n"
               "    -H, --host TEXT      Interface IP to bind [default: 127.0.0.1]\n"
               "    -a, --auth TEXT      HTTP Basic Auth credentials in 'username:password' format\n"
               "    -b, --browser        Auto-open default system browser\n"
               "    --max-clients INT    Maximum concurrent client sessions [default: 5]\n\n"
               "For help on a specific subcommand, run: grocycle <subcommand> --help\n");

    // Parse CLI arguments
    CLI11_PARSE(app, argc, argv);

    if (config_path.empty()) {
        config_path = grocycle::Config::default_user_config_path();
    }
    grocycle::Config cfg = grocycle::Config::load_from_file(config_path);

    if (top_sub->parsed()) {
        return grocycle::cli::run_top_command(cfg, window, path_filter, limit, format);
    }
    if (service_sub->parsed()) {
        return grocycle::cli::run_service_command(cfg, service_action);
    }
    if (config_sub->parsed()) {
        return grocycle::cli::run_config_command(cfg, config_path, config_action, val1, val2);
    }
    if (clean_sub->parsed()) {
        return grocycle::cli::run_clean_command(cfg, force_clean);
    }
    if (web_sub->parsed()) {
        return grocycle::cli::run_web_command(cfg, config_path, web_port, web_host, web_auth, open_browser,
                                              max_clients);
    }
    if (tui_sub->parsed()) {
        return grocycle::tui::launch_tui(cfg);
    }

    // Default: Interactive TUI
    return grocycle::tui::launch_tui(cfg);
}
