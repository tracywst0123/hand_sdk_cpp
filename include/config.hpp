#pragma once

#include <cstdint>
#include <string>

namespace hand_sdk
{

// Distributed Clocks (DC-Synchron) parameters.
//
// SYNC0-only device: each ESC acts on its own drift-corrected SYNC0 pulse
// rather than on frame arrival. Register details and the reasoning behind
// every default here: notes.md, "DC-Synchron bring-up".
struct DcConfig
{
    // Set false to leave the ESCs in SM-Synchron / FreeRun (no SYNC pulses).
    bool enabled{true};

    // SYNC0 period. Must equal the master cycle period; device floor 100 us.
    uint32_t sync0_cycle_ns{1'000'000};

    // SYNC0 offset from the DC grid boundary.
    int32_t sync0_shift_ns{100'000};

    // SYNC1 offset from the DC grid boundary (same cycle as SYNC0).
    int32_t sync1_shift_from_sync0_ns{200'000};

    // Target phase of the master task inside the DC cycle: after SYNC0, and
    // clear of sync0_shift_ns by more than the phase jitter.
    int64_t master_offset_ns{400'000};

    // Write 0x1C32:01 / 0x1C33:01 in PREOP so the device acts on SYNC0.
    // Non-fatal: devices that hardwire the sync type reject it; we log the
    // readback either way.
    bool write_sync_type{true};

    // 0 = FreeRun, 1 = SM-Synchron, 2 = DC SYNC0, 3 = DC SYNC1.
    uint16_t out_sync_type{2};
    uint16_t in_sync_type{2};
};

struct Config
{
    std::string interface_name{"enp132s0"};
    std::string left_hand_name{"hand_200_L"};
    std::string right_hand_name{"hand_200_R"};
    DcConfig dc{};

    // Push a slave that has fallen out of OP back towards it, from the cyclic
    // thread, instead of leaving the run dead until the next connect().
    //
    // A dropout is the slave's own doing -- the SM watchdog or a sync error
    // demotes it to SAFE_OP+ERROR without asking the master -- and nothing in
    // the cyclic loop used to notice. Since SAFE_OP disables the output
    // SyncManager, the FPWR stops being counted and every later cycle reports a
    // short working counter, so one glitch cost the whole session.
    //
    // Set false while diagnosing: recovery hides how often the bus actually
    // drops, and a run that dies at the first fault is easier to read.
    bool auto_recover{true};

    // SCHED_FIFO priority for the cyclic thread. Applied to that thread only —
    // user threads stay at normal priority and remain preemptible by it.
    int rt_priority{80};

    // How far a numeric write service's readback may sit from what was asked
    // before the joint counts as having refused it.
    //
    // Gains (cmd 14) are the only numeric write. The application side is float
    // and the electronics are fixed point, joined by a scale of 2048, so one
    // step is 1/2048 and that is the whole error budget: a request of
    // (1/2048 - delta) is a number the electronics cannot hold, and it runs 0
    // instead -- off by very nearly a full step through nobody's fault. One
    // step of disagreement is therefore the conversion; more than one step is
    // the device substituting a value of its own, which is what this is here to
    // catch. Enum writes -- comm and mode -- are matched exactly and ignore it.
    float service_tolerance{1.0f / 2048.0f};  // ~0.000488
};

} // namespace hand_sdk
