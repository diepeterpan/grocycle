#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>

namespace grocycle::ipc {

using json = nlohmann::json;

// Socket Server for background daemon
class SocketServer {
public:
    using Handler = std::function<json(const json &)>;

    explicit SocketServer(std::string socket_path, Handler handler);
    ~SocketServer();

    bool start();
    void stop();

private:
    std::string socket_path_;
    Handler handler_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

    void run_loop();
    void handle_client(int client_fd);
};

// Socket Client for CLI / TUI to communicate with daemon
class SocketClient {
public:
    explicit SocketClient(std::string socket_path);

    bool is_daemon_running();
    json send_command(const json &request, int timeout_sec = 5);

private:
    std::string socket_path_;
};

} // namespace grocycle::ipc
