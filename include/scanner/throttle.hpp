#pragma once

#include <cstdint>
#include <chrono>

namespace grocycle::scanner {

class Throttle {
public:
    explicit Throttle(uint32_t step_count = 250, uint32_t sleep_ms = 15);

    // Call after examining a directory or batch of files. Sleeps if threshold reached.
    void tick();

    // Sets system ionice to idle and process nice to 19
    static bool apply_low_priority_scheduling();

    void reset();

private:
    uint32_t step_count_;
    uint32_t sleep_ms_;
    uint32_t counter_ = 0;
};

} // namespace grocycle::scanner
