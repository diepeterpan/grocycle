#include "common/utils.hpp"
#include <iomanip>
#include <sstream>
#include <cmath>
#include <filesystem>
#include <cctype>

namespace fs = std::filesystem;

namespace grocycle::utils {

std::string format_bytes(int64_t bytes, bool show_sign) {
    const char *suffixes[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    bool is_negative = bytes < 0;
    double count = static_cast<double>(std::abs(bytes));
    int s = 0;
    while (count >= 1024.0 && s < 5) {
        count /= 1024.0;
        s++;
    }

    std::ostringstream ss;
    if (show_sign) {
        if (is_negative)
            ss << "-";
        else if (bytes > 0)
            ss << "+";
    } else if (is_negative) {
        ss << "-";
    }

    if (s == 0) {
        ss << static_cast<int64_t>(count) << " " << suffixes[s];
    } else {
        ss << std::fixed << std::setprecision(count < 10.0 ? 2 : 1) << count << " " << suffixes[s];
    }
    return ss.str();
}

std::string format_duration(uint32_t seconds) {
    if (seconds >= 86400) {
        uint32_t days = seconds / 86400;
        uint32_t hours = (seconds % 86400) / 3600;
        if (hours == 0)
            return std::to_string(days) + "d";
        return std::to_string(days) + "d " + std::to_string(hours) + "h";
    }
    if (seconds >= 3600) {
        uint32_t hours = seconds / 3600;
        uint32_t mins = (seconds % 3600) / 60;
        if (mins == 0)
            return std::to_string(hours) + "h";
        return std::to_string(hours) + "h " + std::to_string(mins) + "m";
    }
    uint32_t mins = seconds / 60;
    return std::to_string(mins) + "m";
}

bool parse_duration(const std::string &input, uint32_t &out_seconds) {
    if (input.empty())
        return false;

    std::string val_str;
    std::string unit_str;
    size_t i = 0;
    while (i < input.size() && (std::isdigit(input[i]) || input[i] == '.')) {
        val_str += input[i++];
    }
    while (i < input.size()) {
        if (!std::isspace(input[i])) {
            unit_str += std::tolower(input[i]);
        }
        i++;
    }

    if (val_str.empty())
        return false;
    double val = 0.0;
    try {
        val = std::stod(val_str);
    } catch (...) {
        return false;
    }

    if (unit_str == "m" || unit_str == "min" || unit_str == "mins" || unit_str == "minutes") {
        out_seconds = static_cast<uint32_t>(val * 60);
    } else if (unit_str == "h" || unit_str == "hr" || unit_str == "hrs" || unit_str == "hours") {
        out_seconds = static_cast<uint32_t>(val * 3600);
    } else if (unit_str == "d" || unit_str == "day" || unit_str == "days") {
        out_seconds = static_cast<uint32_t>(val * 86400);
    } else if (unit_str == "s" || unit_str == "sec" || unit_str == "secs" || unit_str.empty()) {
        out_seconds = static_cast<uint32_t>(val);
    } else {
        return false;
    }
    return true;
}

int64_t current_timestamp() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::string format_timestamp(int64_t epoch_sec) {
    std::time_t t = static_cast<std::time_t>(epoch_sec);
    std::tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

bool ensure_parent_dir(const std::string &file_path) {
    try {
        fs::path p(file_path);
        if (p.has_parent_path()) {
            return fs::create_directories(p.parent_path()) || fs::exists(p.parent_path());
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace grocycle::utils
