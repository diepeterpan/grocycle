#include "scanner/throttle.hpp"
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sched.h>
#include <iostream>

#ifndef IOPRIO_CLASS_IDLE
#define IOPRIO_CLASS_IDLE 3
#endif

#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif

#ifndef IOPRIO_PRIO_VALUE
#define IOPRIO_PRIO_VALUE(class, data) (((class) << 13) | (data))
#endif

namespace grocycle::scanner {

Throttle::Throttle(uint32_t step_count, uint32_t sleep_ms) : step_count_(step_count), sleep_ms_(sleep_ms) {}

void Throttle::tick() {
    if (step_count_ == 0 || sleep_ms_ == 0)
        return;
    if (++counter_ >= step_count_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_));
        counter_ = 0;
    }
}

void Throttle::reset() {
    counter_ = 0;
}

bool Throttle::apply_low_priority_scheduling() {
    // 1. Set CPU nice to 19 (lowest priority)
    setpriority(PRIO_PROCESS, 0, 19);

    // 2. Set scheduler to SCHED_IDLE or SCHED_BATCH if possible
    struct sched_param param;
    param.sched_priority = 0;
#ifdef SCHED_IDLE
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
#ifdef SCHED_BATCH
        sched_setscheduler(0, SCHED_BATCH, &param);
#endif
    }
#endif

    // 3. Set I/O priority to IDLE class via ioprio_set syscall
#ifdef __NR_ioprio_set
    int ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 7);
    syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, ioprio);
#endif

    return true;
}

} // namespace grocycle::scanner
