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

    // SCHED_FIFO priority for the cyclic thread. Applied to that thread only —
    // user threads stay at normal priority and remain preemptible by it.
    int rt_priority{80};
};

} // namespace hand_sdk
