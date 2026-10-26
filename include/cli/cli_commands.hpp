#pragma once

#include "common/config.hpp"
#include <string>

namespace grocycle::cli {

int run_top_command(const Config &cfg, const std::string &window_str, const std::string &path_str, size_t limit,
                    const std::string &format);

int run_service_command(const Config &cfg, const std::string &action, const std::string &extra_arg = "");

int run_config_command(Config &cfg, const std::string &config_file, const std::string &action,
                       const std::string &val1 = "", const std::string &val2 = "");

int run_clean_command(const Config &cfg, bool force);

int run_web_command(const Config &cfg, const std::string &config_file, uint16_t port, const std::string &host,
                    const std::string &auth, bool open_browser, size_t max_clients);

} // namespace grocycle::cli
