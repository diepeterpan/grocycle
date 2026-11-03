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
#include <cstdio>
#include <climits>
#include <filesystem>

namespace fs = std::filesystem;

using namespace ftxui;

namespace grocycle::tui {

struct WindowOption {
    std::string label;
    uint32_t seconds;
};

const std::vector<WindowOption> kWindowOptions = {{"10m", 600},   {"1h", 3600},     {"6h", 21600}, {"24h", 86400},
                                                  {"7d", 604800}, {"30d", 2592000}, {"custom", 0}};

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
    bool show_custom_modal = false;
    bool show_top10_modal = false;

    // Top 10 files live scan state
    struct FileEntry {
        std::string filename;
        uint64_t size;
        int64_t mtime;
    };
    std::string top10_target_path = "";
    bool top10_path_exists = true;
    std::string top10_error_msg = "";
    std::vector<FileEntry> top10_files;

    auto scan_top10_live_files = [&]() {
        top10_target_path = path_name_stack.empty() ? "/" : path_name_stack.back();
        top10_files.clear();
        top10_error_msg.clear();
        top10_path_exists = true;

        std::error_code ec;
        if (!fs::exists(top10_target_path, ec) || !fs::is_directory(top10_target_path, ec)) {
            top10_path_exists = false;
            top10_error_msg = "Directory does not exist on disk: " + top10_target_path;
            return;
        }

        std::vector<FileEntry> all_files;
        fs::directory_iterator dit(top10_target_path, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            top10_error_msg = "Cannot access directory (" + ec.message() + ")";
            return;
        }

        for (const auto &entry : dit) {
            std::error_code fec;
            if (entry.is_regular_file(fec)) {
                uint64_t sz = entry.file_size(fec);
                if (!fec) {
                    auto ftime = entry.last_write_time(fec);
                    int64_t mtime = 0;
                    if (!fec) {
                        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                        mtime = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
                    }
                    all_files.push_back({entry.path().filename().string(), sz, mtime});
                }
            }
        }

        std::sort(all_files.begin(), all_files.end(), [](const FileEntry &a, const FileEntry &b) {
            return a.size > b.size;
        });

        if (all_files.size() > 10) {
            all_files.resize(10);
        }
        top10_files = std::move(all_files);
    };

    // Custom snapshot comparison state
    int64_t custom_start_snap_id = -1;
    int64_t custom_end_snap_id = -1;
    std::string custom_range_summary = "";

    std::vector<db::SnapshotRecord> modal_snapshots;
    std::vector<std::string> modal_snapshot_labels;
    int modal_selected_start_idx = 0;
    int modal_selected_end_idx = 0;
    std::string modal_error_msg = "";
    std::function<void()> normalize_modal_selection;

    auto load_snapshots_for_modal = [&]() {
        modal_snapshots = storage.get_recent_snapshots(current_root_id, 100);
        modal_snapshot_labels.clear();
        for (const auto &s : modal_snapshots) {
            std::ostringstream ss;
            ss << "#" << s.id << " — " << utils::format_timestamp(s.started_at) << " ("
               << utils::format_bytes(s.total_allocated_bytes) << ", " << s.total_inodes << " inodes)";
            modal_snapshot_labels.push_back(ss.str());
        }
        if (modal_snapshots.size() >= 2) {
            modal_selected_start_idx = 1; // 2nd newest as baseline
            modal_selected_end_idx = 0;   // newest as target
        } else {
            modal_selected_start_idx = 0;
            modal_selected_end_idx = 0;
        }
        modal_error_msg.clear();
    };

    load_snapshots_for_modal();

    std::string status_msg = "Ready. Use Up/Down, Enter to drill down, Backspace to go up.";

    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);
    ipc::SocketClient client(cfg.socket_path);

    std::vector<db::GrowthEntry> items;
    int64_t current_earlier_snap_ts = 0;
    int64_t current_later_snap_ts = 0;
    int64_t current_earlier_snap_id = 0;
    int64_t current_later_snap_id = 0;

    auto refresh_data = [&]() {
        int64_t earlier_snap_id = 0;
        int64_t later_snap_id = 0;
        int64_t earlier_snap_ts = 0;
        int64_t later_snap_ts = 0;
        std::string range_label = "";

        if (selected_window_idx == 6 && custom_start_snap_id > 0 && custom_end_snap_id > 0) {
            earlier_snap_id = custom_start_snap_id;
            later_snap_id = custom_end_snap_id;
            range_label = custom_range_summary;

            auto s1 = storage.get_snapshot_near_timestamp(current_root_id, 0); // fallback or find by id
            for (const auto &s : modal_snapshots) {
                if (s.id == earlier_snap_id)
                    earlier_snap_ts = s.started_at;
                if (s.id == later_snap_id)
                    later_snap_ts = s.started_at;
            }
        } else {
            auto latest_snap = storage.get_latest_snapshot(current_root_id);
            if (!latest_snap.has_value()) {
                items.clear();
                current_earlier_snap_ts = 0;
                current_later_snap_ts = 0;
                current_earlier_snap_id = 0;
                current_later_snap_id = 0;
                status_msg = "No completed snapshots found yet. Waiting for initial scan...";
                return;
            }

            later_snap_id = latest_snap->id;
            later_snap_ts = latest_snap->started_at;
            uint32_t win_sec = kWindowOptions[selected_window_idx].seconds;
            int64_t target_time = latest_snap->started_at - win_sec;
            auto earlier_snap = storage.get_snapshot_near_timestamp(current_root_id, target_time);
            if (earlier_snap.has_value()) {
                earlier_snap_id = earlier_snap->id;
                earlier_snap_ts = earlier_snap->started_at;
            } else {
                earlier_snap_id = latest_snap->id;
                earlier_snap_ts = latest_snap->started_at;
            }
        }

        current_earlier_snap_id = earlier_snap_id;
        current_later_snap_id = later_snap_id;
        current_earlier_snap_ts = earlier_snap_ts;
        current_later_snap_ts = later_snap_ts;

        items = storage.get_children_growth(current_root_id, earlier_snap_id, later_snap_id, current_path_id);

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
           << earlier_snap_id << " -> #" << later_snap_id;
        if (!range_label.empty()) {
            ss << " (" << range_label << ")";
        }
        ss << " [" << items.size() << " entries]";
        status_msg = ss.str();
    };

    refresh_data();

    // Create base main renderer component
    auto main_renderer = Renderer([&] {
        // Find terminal dimension
        int term_height = screen.dimy();
        // Reserve rows for header (2), path (1), toolbar (1), dates (1), separator (2), table header (1), footer (2)
        int visible_rows = std::max(5, term_height - 10);

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
                  filler(), text("[M/F2: Settings] [Q: Quit] ") | color(Color::Cyan)});

        // Path Breadcrumb with right-aligned [T: Top 10 live] button
        auto breadcrumbs =
            hbox({text("Path: ") | bold, text(path_name_stack.back()) | color(Color::YellowLight) | bold, filler(),
                  text("[T: Top 10 live]") | bold | color(Color::Cyan)});

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

        // Date range display line below Window selector
        std::string start_date_str =
            (current_earlier_snap_ts > 0) ? utils::format_timestamp(current_earlier_snap_ts) : "N/A";
        std::string end_date_str = (current_later_snap_ts > 0) ? utils::format_timestamp(current_later_snap_ts) : "N/A";

        Element dates_line =
            hbox({text("Range:  ") | bold | color(Color::Cyan), text(start_date_str) | color(Color::YellowLight),
                  text(" (#" + std::to_string(current_earlier_snap_id) + ")") | dim,
                  text("  ->  ") | bold | color(Color::White), text(end_date_str) | color(Color::YellowLight),
                  text(" (#" + std::to_string(current_later_snap_id) + ")") | dim});

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

        auto main_view = vbox({header, separator(), breadcrumbs, toolbar, dates_line, separator(), col_headers,
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
                hbox({text("Retention:     "), text(std::to_string(cfg.retention_days) + " days") | bold}),
                hbox({text("Last Scan:     "), text(last_scan_str) | color(Color::White)}),
                hbox({text("Next Scan:     "), text(next_scan_str) | color(Color::White)})};

            // Active Scan Telemetry Section
            if (is_scanning) {
                modal_elements.push_back(separator());
                modal_elements.push_back(text(" Current Scan Progress ") | bold | color(Color::Yellow));

                // Elapsed time
                int64_t elapsed_sec = (scan_started_at > 0) ? (now_ts - scan_started_at) : 0;
                modal_elements.push_back(
                    hbox({text("  Elapsed Time:  "),
                          text(utils::format_elapsed_time(elapsed_sec)) | bold | color(Color::Yellow)}));

                // Dirs & files counts
                std::string counts_str =
                    std::to_string(dirs_scanned) + " dirs, " + std::to_string(files_scanned) + " files";
                modal_elements.push_back(
                    hbox({text("  Scanned:       "), text(counts_str) | bold | color(Color::White)}));

                // Truncate current path if too long
                if (!current_path.empty()) {
                    std::string disp_path = current_path;
                    if (disp_path.length() > 42) {
                        disp_path = "..." + disp_path.substr(disp_path.length() - 39);
                    }
                    modal_elements.push_back(hbox({text("  Current Path:  "), text(disp_path) | dim}));
                }

                // Estimated progress percentage
                if (last_scan_total_dirs > 0) {
                    float pct = static_cast<float>(dirs_scanned) / static_cast<float>(last_scan_total_dirs);
                    if (pct > 0.99f)
                        pct = 0.99f;
                    if (pct < 0.0f)
                        pct = 0.0f;
                    int pct_int = static_cast<int>(pct * 100.0f);

                    std::string gauge_label = std::to_string(pct_int) + "% (~" + std::to_string(dirs_scanned) + "/" +
                                              std::to_string(last_scan_total_dirs) + " dirs)";

                    modal_elements.push_back(
                        hbox({text("  Progress:      "), gauge(pct) | color(Color::Cyan) | size(WIDTH, EQUAL, 20),
                              text(" " + gauge_label) | color(Color::CyanLight)}));
                } else {
                    modal_elements.push_back(
                        hbox({text("  Progress:      "),
                              text("Initial baseline scan (measuring totals)...") | color(Color::CyanLight)}));
                }
            }

            modal_elements.push_back(separator());
            modal_elements.push_back(text("Actions:") | bold);
            modal_elements.push_back(text("  [S] Trigger Scan Now"));
            modal_elements.push_back(text("  [ESC/M] Close this dialog"));
            modal_elements.push_back(separator());
            modal_elements.push_back(text("Press [ESC] or [M] to return to explorer.") | dim | center);

            auto modal_content = vbox(std::move(modal_elements)) | size(WIDTH, EQUAL, 64) |
                                 bgcolor(Color::RGB(20, 20, 24)) | color(Color::White) | borderHeavy |
                                 color(Color::Cyan) | clear_under;

            return dbox({main_view | dim, modal_content | center});
        }

        return main_view;
    });

    // Custom comparison modal: two stacked, fixed-height (5-row) scrollable lists.
    // The dialog never changes height, so web UI mouse targets stay on stable rows.
    //
    // Private mouse key sequence (sent by the web UI, never produced by a keyboard):
    //   ESC [ 77 ; <list> ; <arg> z
    //     list: 0 = Start list, 1 = End list
    //     arg : 0..4 = select visible row n, 100 = scroll up, 101 = scroll down
    constexpr int kModalVisibleRows = 5;
    int modal_start_scroll = 0;
    int modal_end_scroll = 0;
    int modal_focus = 0; // 0 = Start list, 1 = End list

    // Start list: every scan that has at least one strictly later scan.
    auto modal_start_candidates = [&]() {
        std::vector<int> c;
        int64_t max_ts = INT64_MIN;
        for (const auto &s : modal_snapshots)
            max_ts = std::max(max_ts, s.started_at);
        for (int i = 0; i < static_cast<int>(modal_snapshots.size()); ++i) {
            if (modal_snapshots[i].started_at < max_ts)
                c.push_back(i);
        }
        return c;
    };

    // End list: only scans strictly later than the selected Start scan.
    auto modal_end_candidates = [&]() {
        std::vector<int> c;
        if (modal_selected_start_idx < 0 || modal_selected_start_idx >= static_cast<int>(modal_snapshots.size()))
            return c;
        int64_t ts = modal_snapshots[modal_selected_start_idx].started_at;
        for (int i = 0; i < static_cast<int>(modal_snapshots.size()); ++i) {
            if (modal_snapshots[i].started_at > ts)
                c.push_back(i);
        }
        return c;
    };

    auto pos_of = [](const std::vector<int> &c, int idx) {
        auto it = std::find(c.begin(), c.end(), idx);
        return it == c.end() ? -1 : static_cast<int>(it - c.begin());
    };

    auto bound_scroll = [&](int &scroll, int n) {
        scroll = std::max(0, std::min(scroll, std::max(0, n - kModalVisibleRows)));
    };

    auto ensure_visible = [&](int &scroll, int pos, int n) {
        if (pos >= 0) {
            if (pos < scroll)
                scroll = pos;
            if (pos >= scroll + kModalVisibleRows)
                scroll = pos - kModalVisibleRows + 1;
        }
        bound_scroll(scroll, n);
    };

    // Keep selections valid (End follows Start) and selected rows visible.
    normalize_modal_selection = [&]() {
        auto sc = modal_start_candidates();
        if (sc.empty())
            return;
        if (pos_of(sc, modal_selected_start_idx) < 0)
            modal_selected_start_idx = sc.front();
        auto ec = modal_end_candidates();
        if (!ec.empty() && pos_of(ec, modal_selected_end_idx) < 0)
            modal_selected_end_idx = ec.front();
        ensure_visible(modal_start_scroll, pos_of(sc, modal_selected_start_idx), static_cast<int>(sc.size()));
        ensure_visible(modal_end_scroll, pos_of(ec, modal_selected_end_idx), static_cast<int>(ec.size()));
    };

    auto modal_move_selection = [&](int list, int delta) {
        auto c = (list == 0) ? modal_start_candidates() : modal_end_candidates();
        if (c.empty())
            return;
        int &sel = (list == 0) ? modal_selected_start_idx : modal_selected_end_idx;
        int pos = std::max(0, pos_of(c, sel));
        pos = std::max(0, std::min(static_cast<int>(c.size()) - 1, pos + delta));
        sel = c[pos];
        modal_error_msg.clear();
        normalize_modal_selection();
    };

    auto modal_select_visible_row = [&](int list, int row) {
        auto c = (list == 0) ? modal_start_candidates() : modal_end_candidates();
        int scroll = (list == 0) ? modal_start_scroll : modal_end_scroll;
        int pos = scroll + row;
        modal_focus = list;
        if (pos < 0 || pos >= static_cast<int>(c.size()))
            return;
        ((list == 0) ? modal_selected_start_idx : modal_selected_end_idx) = c[pos];
        modal_error_msg.clear();
        normalize_modal_selection();
    };

    auto modal_scroll_list = [&](int list, int delta) {
        auto c = (list == 0) ? modal_start_candidates() : modal_end_candidates();
        int &scroll = (list == 0) ? modal_start_scroll : modal_end_scroll;
        scroll += delta;
        bound_scroll(scroll, static_cast<int>(c.size()));
    };

    auto do_apply_custom_comparison = [&] {
        if (modal_snapshots.empty()) {
            modal_error_msg = "No snapshots recorded yet.";
            return;
        }
        if (modal_selected_start_idx < 0 || modal_selected_start_idx >= static_cast<int>(modal_snapshots.size()) ||
            modal_selected_end_idx < 0 || modal_selected_end_idx >= static_cast<int>(modal_snapshots.size())) {
            modal_error_msg = "Please select valid snapshots.";
            return;
        }

        const auto &start_snap = modal_snapshots[modal_selected_start_idx];
        const auto &end_snap = modal_snapshots[modal_selected_end_idx];

        if (start_snap.started_at >= end_snap.started_at) {
            modal_error_msg = "Error: Start scan must be strictly earlier than End scan.";
            return;
        }

        custom_start_snap_id = start_snap.id;
        custom_end_snap_id = end_snap.id;

        int64_t diff_sec = end_snap.started_at - start_snap.started_at;
        custom_range_summary = utils::format_duration(static_cast<uint32_t>(diff_sec));

        selected_window_idx = 6; // [7:custom]
        show_custom_modal = false;
        refresh_data();
    };

    // Renders one fixed-height list block: title, [▲ Up], 5 rows, [▼ Down] (8 lines total)
    auto render_scan_list = [&](const std::string &title, int list) -> Element {
        auto c = (list == 0) ? modal_start_candidates() : modal_end_candidates();
        int sel = (list == 0) ? modal_selected_start_idx : modal_selected_end_idx;
        int scroll = (list == 0) ? modal_start_scroll : modal_end_scroll;
        int n = static_cast<int>(c.size());
        bool focused = (modal_focus == list);

        Elements rows;
        rows.push_back(text((focused ? "» " : "  ") + title) | bold | color(focused ? Color::Cyan : Color::White));

        Decorator up_style = (scroll > 0) ? color(Color::Cyan) : Decorator(dim);
        rows.push_back(hbox({filler(), text("[▲ Up]") | up_style, filler()}));

        for (int r = 0; r < kModalVisibleRows; ++r) {
            int pos = scroll + r;
            if (pos < n) {
                int idx = c[pos];
                bool is_sel = (idx == sel);
                auto row = text((is_sel ? "▶ " : "  ") + modal_snapshot_labels[idx]);
                if (is_sel)
                    row = focused ? (row | inverted | bold) : (row | bold | color(Color::GreenLight));
                rows.push_back(row);
            } else if (n == 0 && r == 0) {
                rows.push_back(text("  (no later scans available)") | dim);
            } else {
                rows.push_back(text(" "));
            }
        }

        std::string pos_str = "  (" + std::to_string(n > 0 ? pos_of(c, sel) + 1 : 0) + "/" + std::to_string(n) + ")";
        Decorator down_style = (scroll + kModalVisibleRows < n) ? color(Color::Cyan) : Decorator(dim);
        rows.push_back(hbox({filler(), text("[▼ Down]") | down_style, text(pos_str) | dim, filler()}));
        return vbox(std::move(rows));
    };

    auto custom_modal_renderer = Renderer([&] {
        Elements modal_els;
        modal_els.push_back(text(" Custom Snapshot Comparison ") | bold | center | color(Color::Cyan));
        modal_els.push_back(separator());

        if (modal_snapshots.size() < 2 || modal_start_candidates().empty()) {
            modal_els.push_back(text("At least 2 completed scans (at different times) are required.") |
                                color(Color::YellowLight));
            modal_els.push_back(separator());
            modal_els.push_back(hbox({filler(), text("[ [C/ESC] Cancel ]") | bold, filler()}));
        } else {
            modal_els.push_back(render_scan_list("Start Scan (Earlier / Baseline):", 0));
            modal_els.push_back(separator());
            modal_els.push_back(render_scan_list("End Scan (Later / Target):", 1));
            modal_els.push_back(separator());

            // Fixed-height status lines (always present to keep layout stable)
            Element diff_line = text(" ");
            if (modal_selected_start_idx >= 0 && modal_selected_start_idx < static_cast<int>(modal_snapshots.size()) &&
                modal_selected_end_idx >= 0 && modal_selected_end_idx < static_cast<int>(modal_snapshots.size())) {
                const auto &s1 = modal_snapshots[modal_selected_start_idx];
                const auto &s2 = modal_snapshots[modal_selected_end_idx];
                if (s1.started_at < s2.started_at) {
                    int64_t dt = s2.started_at - s1.started_at;
                    diff_line = hbox(
                        {text("Time Difference: ") | bold,
                         text(utils::format_duration(static_cast<uint32_t>(dt))) | color(Color::GreenLight) | bold});
                }
            }
            modal_els.push_back(diff_line);
            modal_els.push_back(modal_error_msg.empty() ? text(" ")
                                                        : text(modal_error_msg) | color(Color::RedLight) | bold);

            modal_els.push_back(separator());
            modal_els.push_back(hbox({filler(), text("[ [A] Apply Comparison ]") | bold | color(Color::GreenLight),
                                      text("     "), text("[ [C/ESC] Cancel ]") | bold, filler()}));
            modal_els.push_back(separator());
            modal_els.push_back(text("Tab/←→: switch list | ↑↓ PgUp/PgDn: move | A/Enter: apply | Esc: close") | dim |
                                center);
        }

        return vbox(std::move(modal_els)) | size(WIDTH, EQUAL, 72) | bgcolor(Color::RGB(20, 20, 24)) |
               color(Color::White) | borderHeavy | color(Color::Cyan) | clear_under;
    });

    auto top10_modal_renderer = Renderer([&] {
        Elements modal_els;
        modal_els.push_back(text(" Top 10 Largest Files (Live Scan) ") | bold | center | color(Color::Cyan));
        modal_els.push_back(separator());

        std::string disp_target = top10_target_path;
        if (disp_target.length() > 58) {
            disp_target = "..." + disp_target.substr(disp_target.length() - 55);
        }
        modal_els.push_back(hbox({text("Directory: ") | bold, text(disp_target) | color(Color::YellowLight) | bold}));
        modal_els.push_back(separator());

        if (!top10_path_exists || !top10_error_msg.empty()) {
            modal_els.push_back(text(""));
            modal_els.push_back(text("  ⚠️  " + top10_error_msg) | bold | color(Color::RedLight));
            modal_els.push_back(text(""));
            modal_els.push_back(separator());
            modal_els.push_back(hbox({filler(), text("[ [C/ESC] Close ]") | bold, filler()}));
            modal_els.push_back(separator());
            modal_els.push_back(text("Press [ESC] or [C] or Click Close to return.") | dim | center);
        } else if (top10_files.empty()) {
            modal_els.push_back(text(""));
            modal_els.push_back(text("  (No regular files found directly in this directory)") | dim | center);
            modal_els.push_back(text(""));
            modal_els.push_back(separator());
            modal_els.push_back(hbox({filler(), text("[ [C/ESC] Close ]") | bold, filler()}));
            modal_els.push_back(separator());
            modal_els.push_back(text("Press [ESC] or [C] or Click Close to return.") | dim | center);
        } else {
            // Header for top 10 files table
            modal_els.push_back(hbox({text(" #   File Name") | bold | flex,
                                      text("Size") | bold | size(WIDTH, EQUAL, 12),
                                      text("Modified") | bold | size(WIDTH, EQUAL, 20)}) |
                                bgcolor(Color::BlueLight) | color(Color::Black));

            for (size_t i = 0; i < top10_files.size(); ++i) {
                const auto &f = top10_files[i];
                std::string rank_str = "#" + std::to_string(i + 1) + " ";
                if (i < 9)
                    rank_str = " #" + std::to_string(i + 1) + " ";

                std::string fn = f.filename;
                if (fn.length() > 36) {
                    fn = fn.substr(0, 33) + "...";
                }

                std::string mtime_str = f.mtime > 0 ? utils::format_timestamp(f.mtime) : "N/A";
                auto row = hbox({text(rank_str) | color(Color::YellowLight), text(fn) | bold | flex,
                                 text(utils::format_bytes(f.size)) | color(Color::GreenLight) | bold |
                                     size(WIDTH, EQUAL, 12),
                                 text(mtime_str) | dim | size(WIDTH, EQUAL, 20)});
                if (i % 2 == 1) {
                    row = row | bgcolor(Color::RGB(28, 28, 34));
                }
                modal_els.push_back(row);
            }

            modal_els.push_back(separator());
            modal_els.push_back(hbox({filler(), text("[ [R] Rescan ]") | bold | color(Color::Cyan), text("     "),
                                      text("[ [C/ESC] Close ]") | bold, filler()}));
            modal_els.push_back(separator());
            modal_els.push_back(text("Press [ESC] or [C] or Click Close to return to explorer.") | dim | center);
        }

        return vbox(std::move(modal_els)) | size(WIDTH, EQUAL, 74) | bgcolor(Color::RGB(20, 20, 24)) |
               color(Color::White) | borderHeavy | color(Color::Cyan) | clear_under;
    });

    auto root_with_custom_modal = Modal(main_renderer, custom_modal_renderer, &show_custom_modal);
    auto root_with_modal = Modal(root_with_custom_modal, top10_modal_renderer, &show_top10_modal);

    // Wrap with CatchEvent decorator
    auto component = CatchEvent(root_with_modal, [&](Event event) {
        if (show_top10_modal) {
            if (event == Event::Escape || event == Event::Character('c') || event == Event::Character('C') ||
                event == Event::Character('q') || event == Event::Character('t') || event == Event::Character('T')) {
                show_top10_modal = false;
                return true;
            }
            if (event == Event::Character('r') || event == Event::Character('R')) {
                scan_top10_live_files();
                return true;
            }
            return true; // Swallow everything else while Top 10 modal is open
        }
        if (show_custom_modal) {
            // Private web UI mouse sequence: ESC [ 77 ; <list> ; <arg> z
            const std::string &in = event.input();
            if (in.size() > 5 && in.rfind("\x1b[77;", 0) == 0 && in.back() == 'z') {
                int list = -1;
                int arg = -1;
                if (std::sscanf(in.c_str() + 5, "%d;%d", &list, &arg) == 2 && (list == 0 || list == 1)) {
                    if (arg >= 0 && arg < kModalVisibleRows)
                        modal_select_visible_row(list, arg);
                    else if (arg == 100)
                        modal_scroll_list(list, -1);
                    else if (arg == 101)
                        modal_scroll_list(list, 1);
                }
                return true;
            }
            if (event == Event::Escape || event == Event::Character('c') || event == Event::Character('C')) {
                show_custom_modal = false;
                return true;
            }
            if (event == Event::Character('a') || event == Event::Character('A') || event == Event::Return) {
                do_apply_custom_comparison();
                return true;
            }
            if (event == Event::Tab || event == Event::TabReverse || event == Event::ArrowLeft ||
                event == Event::ArrowRight) {
                modal_focus = 1 - modal_focus;
                return true;
            }
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                modal_move_selection(modal_focus, -1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                modal_move_selection(modal_focus, 1);
                return true;
            }
            if (event == Event::PageUp) {
                modal_move_selection(modal_focus, -kModalVisibleRows);
                return true;
            }
            if (event == Event::PageDown) {
                modal_move_selection(modal_focus, kModalVisibleRows);
                return true;
            }
            if (event == Event::Home) {
                modal_move_selection(modal_focus, -1000000);
                return true;
            }
            if (event == Event::End) {
                modal_move_selection(modal_focus, 1000000);
                return true;
            }
            return true; // Swallow everything else while the dialog is open
        }

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
        if (event == Event::Character('7') || event == Event::Character('c') || event == Event::Character('C')) {
            load_snapshots_for_modal();
            modal_start_scroll = 0;
            modal_end_scroll = 0;
            modal_focus = 0;
            normalize_modal_selection();
            show_custom_modal = true;
            return true;
        }

        // Top 10 live files dialog hotkey
        if (event == Event::Character('t') || event == Event::Character('T')) {
            scan_top10_live_files();
            show_top10_modal = true;
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
