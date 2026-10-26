#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace grocycle::web {

class PtySession {
public:
    PtySession();
    ~PtySession();

    // Disable copy
    PtySession(const PtySession &) = delete;
    PtySession &operator=(const PtySession &) = delete;

    // Move operations
    PtySession(PtySession &&other) noexcept;
    PtySession &operator=(PtySession &&other) noexcept;

    // Spawns a dedicated PTY child process executing grocycle tui
    bool start(const std::string &config_path = "", int cols = 120, int rows = 40);

    // Updates PTY window size, triggering SIGWINCH in child process
    void set_window_size(int cols, int rows) const;

    // Reads terminal output from the master PTY fd (non-blocking)
    ssize_t read_output(uint8_t *buffer, size_t max_len) const;

    // Writes terminal input/keystrokes to the master PTY fd
    ssize_t write_input(const uint8_t *data, size_t len) const;
    ssize_t write_input(const std::string &data) const;

    int get_master_fd() const {
        return master_fd_;
    }
    pid_t get_child_pid() const {
        return child_pid_;
    }
    bool is_alive() const;

    void close_session();

private:
    int master_fd_{-1};
    pid_t child_pid_{-1};
};

} // namespace grocycle::web
