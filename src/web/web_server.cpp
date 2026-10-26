#include "web/web_server.hpp"
#include "web/base64.hpp"
#include "web/embedded_assets.hpp"
#include "web/pty_session.hpp"
#include "web/sha1.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace grocycle::web {

namespace {

constexpr const char *kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::atomic<bool> g_stop_requested{false};

void sigint_handler(int /*unused*/) {
    g_stop_requested.store(true);
}

bool read_exact(int fd, uint8_t *dest, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = read(fd, dest + received, len - received);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                usleep(1000);
                continue;
            }
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int fd, const void *data, size_t len) {
    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, ptr + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                usleep(1000);
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool send_ws_frame(int fd, const uint8_t *data, size_t len, uint8_t opcode = 0x02) {
    std::vector<uint8_t> header;
    header.reserve(10);
    header.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F))); // FIN + opcode

    if (len <= 125) {
        header.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        header.push_back(126);
        header.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        header.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        header.push_back(127);
        for (int i = 7; i >= 0; --i) {
            header.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    if (!send_all(fd, header.data(), header.size())) {
        return false;
    }
    if (len > 0) {
        return send_all(fd, data, len);
    }
    return true;
}

std::string to_lower(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

WebServer::WebServer(ServerOptions options) : options_(std::move(options)) {}

WebServer::~WebServer() {
    stop();
}

void WebServer::stop() {
    running_.store(false);
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

int WebServer::run() {
    g_stop_requested.store(false);
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);
    std::signal(SIGPIPE, SIG_IGN);

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "Error: Unable to create socket: " << strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(options_.port);

    if (inet_pton(AF_INET, options_.host.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cerr << "Error: Invalid host address: " << options_.host << "\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&serv_addr), sizeof(serv_addr)) < 0) {
        std::cerr << "Error: Failed to bind to " << options_.host << ":" << options_.port << " (" << strerror(errno)
                  << ")\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }

    if (listen(listen_fd_, 16) < 0) {
        std::cerr << "Error: Failed to listen on socket: " << strerror(errno) << "\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }

    running_.store(true);

    std::string display_host = (options_.host == "0.0.0.0") ? "127.0.0.1" : options_.host;
    std::string web_url = "http://" + display_host + ":" + std::to_string(options_.port);

    std::cout << "\n======================================================\n";
    std::cout << "  Grocycle Web Terminal Server Started\n";
    std::cout << "  Interface URL: " << web_url << "\n";
    std::cout << "  Bound Address: " << options_.host << ":" << options_.port << "\n";
    if (!options_.auth.empty()) {
        std::cout << "  Auth Enabled : HTTP Basic Authentication\n";
    }
    std::cout << "  Max Sessions : " << options_.max_clients << " concurrent clients\n";
    std::cout << "  Press Ctrl+C to stop web server.\n";
    std::cout << "======================================================\n\n";

    if (options_.auto_open_browser) {
        std::string cmd = "xdg-open " + web_url + " >/dev/null 2>&1 &";
        int ret = std::system(cmd.c_str());
        (void)ret;
    }

    while (running_.load() && !g_stop_requested.load()) {
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int poll_res = poll(&pfd, 1, 500); // 500ms timeout to check stop flag
        if (poll_res < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_res == 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }

        // Enable TCP_NODELAY for responsive keystrokes
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (active_clients_.load() >= options_.max_clients) {
            std::string resp =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                "Maximum concurrent sessions reached. Please try again later.\n";
            send_all(client_fd, resp.data(), resp.size());
            close(client_fd);
            continue;
        }

        active_clients_++;
        std::thread([this, client_fd]() {
            handle_client(client_fd);
            active_clients_--;
        }).detach();
    }

    std::cout << "\nShutting down Grocycle Web Server...\n";
    stop();
    return 0;
}

void WebServer::handle_client(int client_fd) {
    // Read HTTP request header
    std::string req_buffer;
    req_buffer.reserve(4096);
    char buf[1024];

    while (req_buffer.find("\r\n\r\n") == std::string::npos && req_buffer.size() < 16384) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n <= 0) {
            close(client_fd);
            return;
        }
        req_buffer.append(buf, static_cast<size_t>(n));
    }

    std::istringstream stream(req_buffer);
    std::string method;
    std::string path;
    std::string protocol;
    stream >> method >> path >> protocol;

    // Parse headers
    std::string header_line;
    std::string auth_header;
    std::string ws_key;
    bool is_ws_upgrade = false;

    // Discard remainder of request line
    std::getline(stream, header_line);

    while (std::getline(stream, header_line) && header_line != "\r" && !header_line.empty()) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        auto colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string key = to_lower(header_line.substr(0, colon));
            std::string val = header_line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ')
                val.erase(0, 1);
            while (!val.empty() && val.back() == ' ')
                val.pop_back();

            if (key == "upgrade" && to_lower(val) == "websocket") {
                is_ws_upgrade = true;
            } else if (key == "sec-websocket-key") {
                ws_key = val;
            } else if (key == "authorization") {
                auth_header = val;
            }
        }
    }

    // Verify Basic Auth if configured
    if (!options_.auth.empty()) {
        std::string expected_auth = "Basic " + base64_encode(options_.auth);
        if (auth_header != expected_auth) {
            std::string resp = "HTTP/1.1 401 Unauthorized\r\n"
                               "WWW-Authenticate: Basic realm=\"Grocycle Web\"\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 12\r\n"
                               "Connection: close\r\n\r\n"
                               "Unauthorized";
            send_all(client_fd, resp.data(), resp.size());
            close(client_fd);
            return;
        }
    }

    // Route: Static Assets
    if (path == "/" || path == "/index.html") {
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/html; charset=utf-8\r\n"
             << "Content-Encoding: gzip\r\n"
             << "Content-Length: " << assets::INDEX_HTML_GZ_LEN << "\r\n"
             << "Connection: close\r\n\r\n";
        send_all(client_fd, resp.str().data(), resp.str().size());
        send_all(client_fd, assets::INDEX_HTML_GZ, assets::INDEX_HTML_GZ_LEN);
        close(client_fd);
        return;
    }

    if (path == "/xterm.js") {
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/javascript; charset=utf-8\r\n"
             << "Content-Encoding: gzip\r\n"
             << "Cache-Control: public, max-age=86400\r\n"
             << "Content-Length: " << assets::XTERM_JS_GZ_LEN << "\r\n"
             << "Connection: close\r\n\r\n";
        send_all(client_fd, resp.str().data(), resp.str().size());
        send_all(client_fd, assets::XTERM_JS_GZ, assets::XTERM_JS_GZ_LEN);
        close(client_fd);
        return;
    }

    if (path == "/xterm.css") {
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/css; charset=utf-8\r\n"
             << "Content-Encoding: gzip\r\n"
             << "Cache-Control: public, max-age=86400\r\n"
             << "Content-Length: " << assets::XTERM_CSS_GZ_LEN << "\r\n"
             << "Connection: close\r\n\r\n";
        send_all(client_fd, resp.str().data(), resp.str().size());
        send_all(client_fd, assets::XTERM_CSS_GZ, assets::XTERM_CSS_GZ_LEN);
        close(client_fd);
        return;
    }

    if (path == "/xterm-addon-fit.js") {
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/javascript; charset=utf-8\r\n"
             << "Content-Encoding: gzip\r\n"
             << "Cache-Control: public, max-age=86400\r\n"
             << "Content-Length: " << assets::XTERM_ADDON_FIT_JS_GZ_LEN << "\r\n"
             << "Connection: close\r\n\r\n";
        send_all(client_fd, resp.str().data(), resp.str().size());
        send_all(client_fd, assets::XTERM_ADDON_FIT_JS_GZ, assets::XTERM_ADDON_FIT_JS_GZ_LEN);
        close(client_fd);
        return;
    }

    // Route: WebSocket Upgrade
    if (path == "/ws" && is_ws_upgrade && !ws_key.empty()) {
        std::string accept_key = base64_encode(sha1_digest(ws_key + kWebSocketGuid));
        std::ostringstream ws_resp;
        ws_resp << "HTTP/1.1 101 Switching Protocols\r\n"
                << "Upgrade: websocket\r\n"
                << "Connection: Upgrade\r\n"
                << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";
        if (!send_all(client_fd, ws_resp.str().data(), ws_resp.str().size())) {
            close(client_fd);
            return;
        }

        // Dedicated PTY Session for this connection
        PtySession pty;
        if (!pty.start(options_.config_path, 120, 40)) {
            std::string err_msg = "\r\n\033[1;31mError: Failed to allocate pseudoterminal (forkpty).\033[0m\r\n";
            send_ws_frame(client_fd, reinterpret_cast<const uint8_t *>(err_msg.data()), err_msg.size(), 0x02);
            close(client_fd);
            return;
        }

        uint8_t pty_buf[4096];

        while (running_.load() && !g_stop_requested.load() && pty.is_alive()) {
            struct pollfd pfds[2];
            pfds[0].fd = client_fd;
            pfds[0].events = POLLIN;
            pfds[1].fd = pty.get_master_fd();
            pfds[1].events = POLLIN;

            int ret = poll(pfds, 2, 500);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (ret == 0) {
                continue;
            }

            // Check errors
            if (((pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) ||
                ((pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
                break;
            }

            // 1. Data from PTY -> forward to WebSocket
            if ((pfds[1].revents & POLLIN) != 0) {
                ssize_t n = pty.read_output(pty_buf, sizeof(pty_buf));
                if (n > 0) {
                    if (!send_ws_frame(client_fd, pty_buf, static_cast<size_t>(n), 0x02)) {
                        break;
                    }
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    break;
                }
            }

            // 2. Data from WebSocket -> forward to PTY
            if ((pfds[0].revents & POLLIN) != 0) {
                uint8_t frame_hdr[2];
                if (!read_exact(client_fd, frame_hdr, 2)) {
                    break;
                }

                uint8_t opcode = frame_hdr[0] & 0x0F;
                bool masked = (frame_hdr[1] & 0x80) != 0;
                uint64_t payload_len = frame_hdr[1] & 0x7F;

                if (opcode == 0x08) {
                    // Close frame
                    uint8_t close_resp[] = {0x88, 0x00};
                    send_all(client_fd, close_resp, sizeof(close_resp));
                    break;
                }
                if (opcode == 0x09) {
                    // Ping -> Pong
                    uint8_t pong_resp[] = {0x8A, 0x00};
                    send_all(client_fd, pong_resp, sizeof(pong_resp));
                    continue;
                }

                if (payload_len == 126) {
                    uint8_t ext[2];
                    if (!read_exact(client_fd, ext, 2))
                        break;
                    payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
                } else if (payload_len == 127) {
                    uint8_t ext[8];
                    if (!read_exact(client_fd, ext, 8))
                        break;
                    payload_len = 0;
                    for (int i = 0; i < 8; ++i) {
                        payload_len = (payload_len << 8) | ext[i];
                    }
                }

                uint8_t mask_key[4] = {0, 0, 0, 0};
                if (masked) {
                    if (!read_exact(client_fd, mask_key, 4))
                        break;
                }

                std::vector<uint8_t> payload(payload_len);
                if (payload_len > 0) {
                    if (!read_exact(client_fd, payload.data(), payload_len)) {
                        break;
                    }
                    if (masked) {
                        for (size_t i = 0; i < payload_len; ++i) {
                            payload[i] ^= mask_key[i % 4];
                        }
                    }
                }

                if (opcode == 0x01) {
                    // Text payload: check if resize JSON
                    std::string text(reinterpret_cast<const char *>(payload.data()), payload.size());
                    if (text.find("\"resize\"") != std::string::npos) {
                        try {
                            auto j = nlohmann::json::parse(text, nullptr, false);
                            if (!j.is_discarded() && j.value("type", "") == "resize") {
                                int cols = j.value("cols", 120);
                                int rows = j.value("rows", 40);
                                pty.set_window_size(cols, rows);
                                continue;
                            }
                        } catch (const std::exception &e) {
                            (void)e;
                        }
                    }
                    pty.write_input(text);
                } else if (opcode == 0x02) {
                    // Binary payload
                    pty.write_input(payload.data(), payload.size());
                }
            }
        }

        pty.close_session();
        close(client_fd);
        return;
    }

    // Default 404
    std::string not_found =
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
    send_all(client_fd, not_found.data(), not_found.size());
    close(client_fd);
}

int run_web_server(const Config &cfg, const ServerOptions &options) {
    (void)cfg;
    WebServer server(options);
    return server.run();
}

} // namespace grocycle::web
