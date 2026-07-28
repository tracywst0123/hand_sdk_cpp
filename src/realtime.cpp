#include "realtime.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace hand_sdk::rt
{
namespace
{

constexpr int64_t kNsPerSec = 1000000000L;

void normalize(timespec& ts)
{
    while (ts.tv_nsec >= kNsPerSec) {
        ts.tv_nsec -= kNsPerSec;
        ++ts.tv_sec;
    }
    while (ts.tv_nsec < 0) {
        ts.tv_nsec += kNsPerSec;
        --ts.tv_sec;
    }
}

} // namespace

std::string configureRealtime(int priority)
{
    std::string problems;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        problems += std::string("mlockall failed (") + std::strerror(errno) + "). ";
    }

    sched_param param{};
    param.sched_priority = priority;
    // pthread_setschedparam returns the error number rather than setting errno.
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        problems += std::string("SCHED_FIFO failed (") + std::strerror(rc) +
                    "); grant it with: sudo setcap cap_net_raw,cap_sys_nice+ep <binary>";
    }

    return problems;
}

CyclicTimer::CyclicTimer(int64_t cycle_ns, int64_t offset_ns)
    : cycle_ns_(cycle_ns), offset_ns_(offset_ns)
{
}

void CyclicTimer::waitNextCycle()
{
    if (!started_) {
        clock_gettime(CLOCK_MONOTONIC, &next_);
        started_ = true;
    }

    next_.tv_nsec += cycle_ns_ + correction_;
    normalize(next_);

    // Absolute deadline, so the work we just did does not stretch the period.
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_, nullptr);
}

void CyclicTimer::syncTo(int64_t dc_time_ns)
{
    if (dc_time_ns == 0) {
        correction_ = 0;
        return;
    }

    // Where in the DC cycle did we actually land, measured from the target phase?
    int64_t delta = (dc_time_ns - offset_ns_) % cycle_ns_;
    if (delta > cycle_ns_ / 2) {
        // Being 900 us "late" is really being 100 us early — nudge forward, not
        // back through almost a whole cycle.
        delta -= cycle_ns_;
    }
    phase_error_ = delta;

    // Proportional term closes the bulk of the error; the integral term removes
    // the residual offset caused by our clock and the reference clock running at
    // slightly different rates. Same shape as ec_sync() in SOEM's simple_test.
    if (delta > 0) {
        ++integral_;
    }
    if (delta < 0) {
        --integral_;
    }
    correction_ = -(delta / 100) - (integral_ / 20);
}

} // namespace hand_sdk::rt
