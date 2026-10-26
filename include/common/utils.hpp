#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace grocycle::utils {

// Formats byte count to human-readable string (e.g., "1.45 GiB", "250.2 MiB")
std::string format_bytes(int64_t bytes, bool show_sign = false);

// Formats duration (e.g. "10m", "2h 15m", "14d")
std::string format_duration(uint32_t seconds);

// Parses human duration (e.g., "10m", "1h", "24h", "7d", "30d") to seconds
bool parse_duration(const std::string &input, uint32_t &out_seconds);

// Get current Unix epoch timestamp in seconds
int64_t current_timestamp();

// Converts timestamp to formatted date/time string
std::string format_timestamp(int64_t epoch_sec);

// Ensures parent directories exist
bool ensure_parent_dir(const std::string &file_path);

} // namespace grocycle::utils
