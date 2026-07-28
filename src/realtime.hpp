#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace hand_sdk::rt
{

// Ask for SCHED_FIFO and lock our pages into RAM, so a browser tab or a page
// fault cannot push the cyclic task past its DC deadline.
//
// Returns an empty string on success, otherwise a human-readable reason. Failure
// is soft: the loop still runs, it just jitters more. Grant the capability with
//   sudo setcap cap_net_raw,cap_sys_nice+ep <binary>
// (cap_net_raw is already needed for the raw EtherCAT socket).
std::string configureRealtime(int priority = 80);

// Phase-locked cyclic timer for a DC-Synchron bus.
//
// Two jobs. First, sleep to *absolute* deadlines on a fixed grid, so the time
// spent doing work stops accumulating into the period the way a plain
// sleep_for(1ms) at the bottom of a loop does. Second, steer those deadlines so
// the task consistently wakes a chosen offset after each DC grid boundary —
// without that, the master's grid and the slaves' DC grid free-run against each
// other and the exchange eventually drifts across a SYNC pulse.
class CyclicTimer
{
public:
    // cycle_ns must equal the SYNC0 period; offset_ns is the target phase within
    // the DC cycle (see DcConfig::master_offset_ns).
    CyclicTimer(int64_t cycle_ns, int64_t offset_ns);

    // Sleep until the next deadline. Call once at the top of every cycle.
    void waitNextCycle();

    // Feed the DC system time sampled during the cycle that just ran; this sets
    // the correction applied to the next deadline. Passing 0 (DC inactive) lets
    // the timer free-run on its own grid.
    void syncTo(int64_t dc_time_ns);

    // Signed distance from the target phase, in ns. Converges towards 0 and
    // should then stay within a few microseconds. Useful to log.
    int64_t phaseError() const { return phase_error_; }

private:
    int64_t cycle_ns_;
    int64_t offset_ns_;
    int64_t correction_{0};
    int64_t integral_{0};
    int64_t phase_error_{0};
    timespec next_{};
    bool started_{false};
};

} // namespace hand_sdk::rt
