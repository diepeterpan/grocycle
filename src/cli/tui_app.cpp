#include "tui/tui_app.hpp"
#include "common/version.hpp"
#include "common/utils.hpp"
#include "db/storage_engine.hpp"
#include "ipc/unix_socket.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <thread>
#include <atomic>

using namespace ftxui;

namespace grocycle::tui {

struct WindowOption {
    std::string label;
    uint32_t seconds;
};

const std::vector<WindowOption> kWindowOptions = {{"10m", 600},   {"1h", 3600},   {"6h", 21600},
                                                  {"24h", 86400}, {"7d", 604800}, {"30d", 2592000}};

enum class SortMode { GrowthDelta, GrowthRate, TotalSize, Alphabetical };

int launch_tui(const Config &cfg) {
    db::StorageEngine storage(cfg.db_path);
    if (!storage.initialize()) {
        std::cerr << "Cannot open database at: " << cfg.db_path << std::endl;
        return 1;
    }

    auto roots = storage.get_monitored_roots();
    if (roots.empty()) {
        std::cerr << "No monitored roots found. Start grocycled first." << std::endl;
        return 1;
    }

    int64_t current_root_id = roots[0].id;
    std::string root_path_str = roots[0].path;

    // Look up the root path ID in database (e.g. for "/")
    int64_t root_path_id = 0;
    auto root_rec = storage.find_path_by_string(root_path_str);
    if (root_rec.has_value()) {
        root_path_id = root_rec->id;
    } else {
        root_path_id = storage.get_or_create_path(root_path_str, 0, root_path_str);
    }

    int64_t current_path_id = root_path_id;
    std::vector<int64_t> nav_stack; // For going up directory tree
    std::vector<std::string> path_name_stack = {root_path_str};

    int selected_window_idx = 1; // Default 1h
    SortMode sort_mode = SortMode::GrowthDelta;
    int selected_item = 0;
    int scroll_offset = 0; // Viewport top index
    bool show_settings_modal = false;
    std::string status_msg = "Ready. Use Up/Down, Enter to drill down, Backspace to go up.";

    auto screen = ScreenInteractive::Fullscreen();
    ipc::SocketClient client(cfg.socket_path);

    std::vector<db::GrowthEntry> items;

    auto refresh_data = [&]() {
        auto latest_snap = storage.get_latest_snapshot(current_root_id);
        if (!latest_snap.has_value()) {
            items.clear();
            status_msg = "No completed snapshots found yet. Waiting for initial scan...";
            return;
        }

        uint32_t win_sec = kWindowOptions[selected_window_idx].seconds;
        int64_t target_time = latest_snap->started_at - win_sec;
        auto earlier_snap = storage.get_snapshot_near_timestamp(current_root_id, target_time);
        int64_t earlier_snap_id = earlier_snap.has_value() ? earlier_snap->id : latest_snap->id;

        items = storage.get_children_growth(current_root_id, earlier_snap_id, latest_snap->id, current_path_id);

        // Sort items
        std::sort(items.begin(), items.end(), [&](const db::GrowthEntry &a, const db::GrowthEntry &b) {
            switch (sort_mode) {
            case SortMode::GrowthDelta:
                return a.delta_allocated > b.delta_allocated;
            case SortMode::GrowthRate:
                return a.bytes_per_hour > b.bytes_per_hour;
            case SortMode::TotalSize:
                return a.current_allocated > b.current_allocated;
            case SortMode::Alphabetical: {
                auto it_a = a.name.begin(), it_b = b.name.begin();
                while (it_a != a.name.end() && it_b != b.name.end()) {
                    char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*it_a)));
                    char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*it_b)));
                    if (ca != cb)
                        return ca < cb;
                    ++it_a;
                    ++it_b;
                }
                return (it_a == a.name.end() && it_b != b.name.end());
            }
            }
            return a.delta_allocated > b.delta_allocated;
        });

        if (selected_item >= static_cast<int>(items.size())) {
            selected_item = std::max(0, static_cast<int>(items.size()) - 1);
        }

        std::ostringstream ss;
        ss << "Viewing: " << (path_name_stack.empty() ? "/" : path_name_stack.back()) << " | Snapshots: #"
           << earlier_snap_id << " -> #" << latest_snap->id << " (" << items.size() << " entries)";
        status_msg = ss.str();
    };

    refresh_data();

    // Create a base renderer component
    auto renderer = Renderer([&] {
        // Find terminal dimension
        int term_height = screen.dimy();
        // Reserve rows for header (2), path (1), toolbar (1), separator (2), table header (1), footer (2)
        int visible_rows = std::max(5, term_height - 9);

        // Adjust scroll offset to keep selected_item visible
        if (selected_item < scroll_offset) {
            scroll_offset = selected_item;
        } else if (selected_item >= scroll_offset + visible_rows) {
            scroll_offset = selected_item - visible_rows + 1;
        }
        scroll_offset =
            std::max(0, std::min(scroll_offset, std::max(0, static_cast<int>(items.size()) - visible_rows)));

        // Find max delta for growth bar rendering
        int64_t max_delta = 1;
        for (const auto &it : items) {
            if (it.delta_allocated > max_delta)
                max_delta = it.delta_allocated;
        }

        // Build Table Rows within the windowed viewport
        Elements rows;
        int end_idx = std::min(static_cast<int>(items.size()), scroll_offset + visible_rows);
        for (int i = scroll_offset; i < end_idx; ++i) {
            const auto &item = items[i];
            bool is_selected = (i == selected_item);

            // Relative growth progress bar
            float ratio =
                std::clamp(static_cast<float>(item.delta_allocated) / static_cast<float>(max_delta), 0.0f, 1.0f);
            int bar_width = 10;
            int filled = static_cast<int>(ratio * bar_width);
            std::string bar = "[" + std::string(filled, '#') + std::string(bar_width - filled, ' ') + "]";

            // Color selection based on growth magnitude
            Color growth_color = Color::Green;
            if (item.delta_allocated > 1024LL * 1024 * 1024) { // > 1 GiB
                growth_color = Color::RedLight;
            } else if (item.delta_allocated > 100LL * 1024 * 1024) { // > 100 MiB
                growth_color = Color::Yellow;
            }

            auto name_element = text((is_selected ? "> " : "  ") + item.name + "/") | (is_selected ? bold : nothing);
            auto size_element = text(utils::format_bytes(item.current_allocated));
            auto delta_element = text(utils::format_bytes(item.delta_allocated, true)) | color(growth_color) | bold;
            auto rate_element =
                text(utils::format_bytes(static_cast<int64_t>(item.bytes_per_hour)) + "/h") | color(growth_color);
            auto bar_element = text(bar) | color(growth_color);

            auto row = hbox({name_element | flex, size_element | size(WIDTH, EQUAL, 14),
                             delta_element | size(WIDTH, EQUAL, 14), rate_element | size(WIDTH, EQUAL, 15),
                             bar_element | size(WIDTH, EQUAL, 13)});

            if (is_selected) {
                row = row | bgcolor(Color::GrayDark);
            }
            rows.push_back(row);
        }

        if (rows.empty()) {
            rows.push_back(text("No subdirectories or data in this view.") | dim | center);
        }

        // Header Title Bar
        std::string ver_badge = " " + std::string(version::FULL_VERSION_STRING) + " ";
        auto header =
            hbox({text(" GROCYCLE ") | bold | bgcolor(Color::Blue) | color(Color::White),
                  text(ver_badge) | bold | color(Color::YellowLight), text("Linux Directory Growth Explorer ") | dim,
                  filler(), text("[M/F2: Daemon Settings] [Q: Quit] ") | color(Color::Cyan)});

        // Path Breadcrumb
        auto breadcrumbs =
            hbox({text("Path: ") | bold, text(path_name_stack.back()) | color(Color::YellowLight) | bold});

        // Window selector tabs
        Elements win_tabs;
        for (int i = 0; i < static_cast<int>(kWindowOptions.size()); ++i) {
            bool active = (i == selected_window_idx);
            std::string label = "[" + std::to_string(i + 1) + ":" + kWindowOptions[i].label + "]";
            auto tab = text(label);
            if (active) {
                tab = tab | bold | color(Color::Black) | bgcolor(Color::Green);
            } else {
                tab = tab | dim;
            }
            win_tabs.push_back(tab);
            win_tabs.push_back(text(" "));
        }
        auto toolbar = hbox({text("Window: ") | bold, hbox(std::move(win_tabs)), filler(),
                             text("Sort: [G]rowth | [R]ate | [S]ize | [N]ame") | dim});

        // Table Columns Header
        auto col_headers =
            hbox({text("  Directory Name") | bold | flex, text("Current Size") | bold | size(WIDTH, EQUAL, 14),
                  text("Growth (Δ)") | bold | size(WIDTH, EQUAL, 14),
                  text("Growth Rate") | bold | size(WIDTH, EQUAL, 15),
                  text("Relative") | bold | size(WIDTH, EQUAL, 13)}) |
            bgcolor(Color::BlueLight) | color(Color::Black);

        // Status bar with item position counter
        std::string pos_info =
            items.empty() ? "" : (" [" + std::to_string(selected_item + 1) + "/" + std::to_string(items.size()) + "] ");
        auto footer = hbox({text(status_msg) | color(Color::White) | dim, text(pos_info) | bold | color(Color::Cyan),
                            filler(), text("Enter: Drilldown | Backspace: Up | Arrows: Move") | dim});

        auto main_view = vbox({header, separator(), breadcrumbs, toolbar, separator(), col_headers,
                               vbox(std::move(rows)) | yflex, separator(), footer});

        if (show_settings_modal) {
            bool is_running = client.is_daemon_running();
            std::string d_status = is_running ? "RUNNING" : "STOPPED";
            Color d_color = is_running ? Color::Green : Color::Red;

            nlohmann::json status_json;
            bool is_scanning = false;
            int64_t last_s = 0;
            int64_t next_s = 0;
            int64_t scan_started_at = 0;
            int64_t dirs_scanned = 0;
            int64_t files_scanned = 0;
            int64_t last_scan_total_dirs = 0;
            std::string current_path = "";

            if (is_running) {
                status_json = client.send_command({{"command", "status"}});
                if (status_json.value("status", "") == "ok") {
                    is_scanning = status_json.value("is_scanning", false);
                    last_s = status_json.value("last_scan_time", 0);
                    next_s = status_json.value("next_scan_time", 0);
                    if (status_json.contains("progress")) {
                        auto p = status_json["progress"];
                        scan_started_at = p.value("scan_started_at", 0);
                        dirs_scanned = p.value("dirs_scanned", 0);
                        files_scanned = p.value("files_scanned", 0);
                        last_scan_total_dirs = p.value("last_scan_total_dirs", 0);
                        current_path = p.value("current_path", "");
                    }
                }
            }

            int64_t now_ts = utils::current_timestamp();

            // Status label
            std::string status_badge = d_status;
            if (is_running) {
                status_badge = is_scanning ? "RUNNING (Active Scan)" : "RUNNING (Idle)";
                d_color = is_scanning ? Color::Yellow : Color::Green;
            }

            // Timestamps formatting
            std::string last_scan_str = "Never";
            if (last_s > 0) {
                int64_t ago = now_ts - last_s;
                last_scan_str = utils::format_timestamp(last_s);
                if (ago >= 0) {
                    last_scan_str += " (" + utils::format_duration(static_cast<uint32_t>(ago)) + " ago)";
                }
            }

            std::string next_scan_str = "Pending initial scan";
            if (next_s > 0) {
                int64_t until = next_s - now_ts;
                next_scan_str = utils::format_timestamp(next_s);
                if (until > 0) {
                    next_scan_str += " (in " + utils::format_duration(static_cast<uint32_t>(until)) + ")";
                } else if (is_scanning) {
                    next_scan_str = "Scanning now";
                } else {
                    next_scan_str += " (due now)";
                }
            }

            Elements modal_elements = {
                text(" Daemon Control & Configuration ") | bold | center | color(Color::Cyan),
                separator(),
                hbox({text("Daemon Status: "), text(status_badge) | bold | color(d_color)}),
                hbox({text("Scan Interval: "), text(utils::format_duration(cfg.scan_interval_seconds)) | bold}),
                hbox({text("Last Scan:     "), text(last_scan_str) | color(Color::White)}),
                hbox({text("Next Scan:     "), text(next_scan_str) | color(Color::White)})
            };

            // Active Scan Telemetry Section
            if (is_scanning) {
                modal_elements.push_back(separator());
                modal_elements.push_back(text(" Current Scan Progress ") | bold | color(Color::Yellow));

                // Elapsed time
                int64_t elapsed_sec = (scan_started_at > 0) ? (now_ts - scan_started_at) : 0;
                modal_elements.push_back(
                    hbox({text("  Elapsed Time:  "), text(utils::format_elapsed_time(elapsed_sec)) | bold | color(Color::Yellow)})
                );

                // Dirs & files counts
                std::string counts_str = std::to_string(dirs_scanned) + " dirs, " + std::to_string(files_scanned) + " files";
                modal_elements.push_back(
                    hbox({text("  Scanned:       "), text(counts_str) | bold | color(Color::White)})
                );

                // Truncate current path if too long
                if (!current_path.empty()) {
                    std::string disp_path = current_path;
                    if (disp_path.length() > 42) {
                        disp_path = "..." + disp_path.substr(disp_path.length() - 39);
                    }
                    modal_elements.push_back(
                        hbox({text("  Current Path:  "), text(disp_path) | dim})
                    );
                }

                // Estimated progress percentage
                if (last_scan_total_dirs > 0) {
                    float pct = static_cast<float>(dirs_scanned) / static_cast<float>(last_scan_total_dirs);
                    if (pct > 0.99f) pct = 0.99f;
                    if (pct < 0.0f) pct = 0.0f;
                    int pct_int = static_cast<int>(pct * 100.0f);

                    std::string gauge_label = std::to_string(pct_int) + "% (~" +
                                              std::to_string(dirs_scanned) + "/" +
                                              std::to_string(last_scan_total_dirs) + " dirs)";

                    modal_elements.push_back(
                        hbox({text("  Progress:      "), gauge(pct) | color(Color::Cyan) | size(WIDTH, EQUAL, 20),
                              text(" " + gauge_label) | color(Color::CyanLight)})
                    );
                } else {
                    modal_elements.push_back(
                        hbox({text("  Progress:      "), text("Initial baseline scan (measuring totals)...") | color(Color::CyanLight)})
                    );
                }
            }

            modal_elements.push_back(separator());
            modal_elements.push_back(text("Actions:") | bold);
            modal_elements.push_back(text("  [S] Trigger Scan Now"));
            modal_elements.push_back(text("  [ESC/M] Close this dialog"));
            modal_elements.push_back(separator());
            modal_elements.push_back(text("Press [ESC] or [M] to return to explorer.") | dim | center);

            auto modal_content =
                vbox(std::move(modal_elements)) |
                size(WIDTH, EQUAL, 64) | bgcolor(Color::RGB(20, 20, 24)) | color(Color::White) | borderHeavy |
                color(Color::Cyan) | clear_under;

            return dbox({main_view | dim, modal_content | center});
        }

        return main_view;
    });

    // Wrap with CatchEvent decorator
    auto component = CatchEvent(renderer, [&](Event event) {
        if (show_settings_modal) {
            if (event == Event::Escape || event == Event::Character('q') || event == Event::Character('m')) {
                show_settings_modal = false;
                return true;
            }
            if (event == Event::Character('s')) {
                client.send_command({{"command", "scan_now"}});
                refresh_data();
                return true;
            }
            return false;
        }

        if (event == Event::Character('q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (selected_item > 0) {
                selected_item--;
            }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (selected_item + 1 < static_cast<int>(items.size())) {
                selected_item++;
            }
            return true;
        }
        if (event == Event::PageUp) {
            selected_item = std::max(0, selected_item - 15);
            return true;
        }
        if (event == Event::PageDown) {
            selected_item = std::min(static_cast<int>(items.size()) - 1, selected_item + 15);
            return true;
        }
        if (event == Event::Home) {
            selected_item = 0;
            return true;
        }
        if (event == Event::End) {
            selected_item = std::max(0, static_cast<int>(items.size()) - 1);
            return true;
        }
        if (event == Event::Return || event == Event::ArrowRight || event == Event::Character('l')) {
            if (!items.empty() && selected_item < static_cast<int>(items.size())) {
                const auto &item = items[selected_item];
                nav_stack.push_back(current_path_id);
                path_name_stack.push_back(item.full_path);
                current_path_id = item.path_id;
                selected_item = 0;
                scroll_offset = 0;
                refresh_data();
            }
            return true;
        }
        if (event == Event::Backspace || event == Event::ArrowLeft || event == Event::Character('h')) {
            if (!nav_stack.empty()) {
                current_path_id = nav_stack.back();
                nav_stack.pop_back();
                path_name_stack.pop_back();
                selected_item = 0;
                scroll_offset = 0;
                refresh_data();
            }
            return true;
        }
        // Time window hotkeys: 1-6
        if (event == Event::Character('1')) {
            selected_window_idx = 0;
            refresh_data();
            return true;
        }
        if (event == Event::Character('2')) {
            selected_window_idx = 1;
            refresh_data();
            return true;
        }
        if (event == Event::Character('3')) {
            selected_window_idx = 2;
            refresh_data();
            return true;
        }
        if (event == Event::Character('4')) {
            selected_window_idx = 3;
            refresh_data();
            return true;
        }
        if (event == Event::Character('5')) {
            selected_window_idx = 4;
            refresh_data();
            return true;
        }
        if (event == Event::Character('6')) {
            selected_window_idx = 5;
            refresh_data();
            return true;
        }

        // Sorting hotkeys
        if (event == Event::Character('g') || event == Event::Character('G')) {
            sort_mode = SortMode::GrowthDelta;
            refresh_data();
            return true;
        }
        if (event == Event::Character('r') || event == Event::Character('R')) {
            sort_mode = SortMode::GrowthRate;
            refresh_data();
            return true;
        }
        if (event == Event::Character('s') || event == Event::Character('S')) {
            sort_mode = SortMode::TotalSize;
            refresh_data();
            return true;
        }
        if (event == Event::Character('n') || event == Event::Character('N')) {
            sort_mode = SortMode::Alphabetical;
            refresh_data();
            return true;
        }
        // Modal / Settings
        if (event == Event::Character('m') || event == Event::Character('M') || event == Event::F2) {
            show_settings_modal = !show_settings_modal;
            return true;
        }
        if (event == Event::Character('r')) {
            refresh_data();
            return true;
        }

        return false;
    });

    std::atomic<bool> tui_running{true};
    std::thread refresh_thread([&]() {
        while (tui_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (show_settings_modal) {
                screen.PostEvent(Event::Custom);
            }
        }
    });

    screen.Loop(component);
    tui_running.store(false);
    if (refresh_thread.joinable()) {
        refresh_thread.join();
    }
    return 0;
}

} // namespace grocycle::tui
