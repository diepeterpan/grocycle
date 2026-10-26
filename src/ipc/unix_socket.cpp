#include "ipc/unix_socket.hpp"
#include "common/utils.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <vector>

namespace grocycle::ipc {

SocketServer::SocketServer(std::string socket_path, Handler handler)
    : socket_path_(std::move(socket_path)), handler_(std::move(handler)) {}

SocketServer::~SocketServer() {
    stop();
}

bool SocketServer::start() {
    stop();

    utils::ensure_parent_dir(socket_path_);
    unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 10) != 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&SocketServer::run_loop, this);
    return true;
}

void SocketServer::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    unlink(socket_path_.c_str());
}

void SocketServer::run_loop() {
    while (running_) {
        struct pollfd pfd;
        pfd.fd = server_fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 500);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(server_fd_, nullptr, nullptr);
            if (client_fd >= 0) {
                handle_client(client_fd);
                close(client_fd);
            }
        }
    }
}

void SocketServer::handle_client(int client_fd) {
    std::string buffer;
    char chunk[4096];
    while (true) {
        ssize_t bytes = read(client_fd, chunk, sizeof(chunk));
        if (bytes <= 0)
            break;
        buffer.append(chunk, bytes);
        if (!buffer.empty() && buffer.back() == '\n')
            break;
    }

    if (buffer.empty())
        return;

    json response;
    try {
        json req = json::parse(buffer);
        if (handler_) {
            response = handler_(req);
        } else {
            response = {{"status", "error"}, {"message", "No handler defined"}};
        }
    } catch (const std::exception &e) {
        response = {{"status", "error"}, {"message", e.what()}};
    }

    std::string out = response.dump() + "\n";
    write(client_fd, out.data(), out.size());
}

SocketClient::SocketClient(std::string socket_path) : socket_path_(std::move(socket_path)) {}

bool SocketClient::is_daemon_running() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    bool ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}

json SocketClient::send_command(const json &request, int timeout_sec) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {{"status", "error"}, {"message", "Could not create socket"}};
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return {{"status", "error"}, {"message", "Daemon is not running or socket unavailable"}};
    }

    std::string req_str = request.dump() + "\n";
    if (write(fd, req_str.data(), req_str.size()) <= 0) {
        close(fd);
        return {{"status", "error"}, {"message", "Failed to write to daemon"}};
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeout_sec * 1000);
    if (ret <= 0) {
        close(fd);
        return {{"status", "error"}, {"message", "Timeout waiting for daemon response"}};
    }

    std::string buffer;
    char chunk[4096];
    while (true) {
        ssize_t bytes = read(fd, chunk, sizeof(chunk));
        if (bytes <= 0)
            break;
        buffer.append(chunk, bytes);
        if (!buffer.empty() && buffer.back() == '\n')
            break;
    }
    close(fd);

    try {
        return json::parse(buffer);
    } catch (const std::exception &e) {
        return {{"status", "error"}, {"message", std::string("Malformed response: ") + e.what()}};
    }
}

} // namespace grocycle::ipc
