#include "web/pty_session.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace grocycle::web {

PtySession::PtySession() = default;

PtySession::~PtySession() {
    close_session();
}

PtySession::PtySession(PtySession &&other) noexcept : master_fd_(other.master_fd_), child_pid_(other.child_pid_) {
    other.master_fd_ = -1;
    other.child_pid_ = -1;
}

PtySession &PtySession::operator=(PtySession &&other) noexcept {
    if (this != &other) {
        close_session();
        master_fd_ = other.master_fd_;
        child_pid_ = other.child_pid_;
        other.master_fd_ = -1;
        other.child_pid_ = -1;
    }
    return *this;
}

bool PtySession::start(const std::string &config_path, int cols, int rows) {
    close_session();

    struct winsize ws{};
    ws.ws_col = cols > 0 ? static_cast<unsigned short>(cols) : 120;
    ws.ws_row = rows > 0 ? static_cast<unsigned short>(rows) : 40;

    pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        // Child process execution
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("LANG", "en_US.UTF-8", 1);

        char exe_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
        } else {
            strncpy(exe_path, "grocycle", sizeof(exe_path) - 1);
            exe_path[sizeof(exe_path) - 1] = '\0';
        }

        std::vector<char *> args;
        args.push_back(exe_path);
        if (!config_path.empty()) {
            args.push_back(const_cast<char *>("-c"));
            args.push_back(const_cast<char *>(config_path.c_str()));
        }
        args.push_back(const_cast<char *>("tui"));
        args.push_back(nullptr);

        execv(exe_path, args.data());
        _exit(127);
    }

    child_pid_ = pid;

    // Set master PTY to non-blocking
    int flags = fcntl(master_fd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    return true;
}

void PtySession::set_window_size(int cols, int rows) const {
    if (master_fd_ >= 0 && cols > 0 && rows > 0) {
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(master_fd_, TIOCSWINSZ, &ws);
    }
}

ssize_t PtySession::read_output(uint8_t *buffer, size_t max_len) const {
    if (master_fd_ < 0) {
        return -1;
    }
    return read(master_fd_, buffer, max_len);
}

ssize_t PtySession::write_input(const uint8_t *data, size_t len) const {
    if (master_fd_ < 0 || len == 0) {
        return -1;
    }
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(master_fd_, data + written, len - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        written += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(written);
}

ssize_t PtySession::write_input(const std::string &data) const {
    return write_input(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

bool PtySession::is_alive() const {
    if (child_pid_ <= 0) {
        return false;
    }
    int status = 0;
    pid_t res = waitpid(child_pid_, &status, WNOHANG);
    return (res == 0);
}

void PtySession::close_session() {
    if (child_pid_ > 0) {
        kill(child_pid_, SIGHUP);
        usleep(5000);
        int status = 0;
        if (waitpid(child_pid_, &status, WNOHANG) == 0) {
            kill(child_pid_, SIGTERM);
            usleep(5000);
            if (waitpid(child_pid_, &status, WNOHANG) == 0) {
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0);
            }
        }
        child_pid_ = -1;
    }
    if (master_fd_ >= 0) {
        close(master_fd_);
        master_fd_ = -1;
    }
}

} // namespace grocycle::web
