#pragma once

#include "common/config.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace grocycle::web {

struct ServerOptions {
    uint16_t port{8080};
    std::string host{"127.0.0.1"};
    std::string auth{""}; // "username:password" (optional)
    bool auto_open_browser{false};
    size_t max_clients{5};
    std::string config_path{""};
};

class WebServer {
public:
    explicit WebServer(ServerOptions options);
    ~WebServer();

    // Starts listening and event loop. Blocks until stopped or SIGINT/SIGTERM.
    int run();

    // Signals server to stop
    void stop();

    bool is_running() const {
        return running_.load();
    }

private:
    ServerOptions options_;
    std::atomic<bool> running_{false};
    int listen_fd_{-1};
    std::atomic<size_t> active_clients_{0};

    void handle_client(int client_fd);
};

// Convenience entry point called from CLI
int run_web_server(const Config &cfg, const ServerOptions &options);

} // namespace grocycle::web
