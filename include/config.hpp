#pragma once

#include "messages.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace hand_sdk
{

// Per-joint installation direction, in logical joint order (finger*4 + joint).
//
// +1 leaves a joint as the model sees it; -1 says the motor is fitted the other
// way round, so the device's positive rotation is the model's negative. The
// flip is applied to q, dq and tau alike and in both directions -- a command
// goes out negated and the state comes back negated -- so everything above Api
// works in one sign convention and nothing else has to know.
//
// Fixed here rather than exposed as a runtime setting: which way a motor is
// fitted is a property of how the hand is built, not something to get wrong at
// launch time. Edit the tables below and rebuild; the SDK, the node and the
// bench app all pick the change up together.
//
// Left and right are separate tables because a mirrored pair does not share a
// sign map: the same joint on the other hand may well be fitted the other way.
//
// Not flipped: gains (magnitudes), the fault bits, and anything on the service
// channel. The zero-calibration sweep has its own direction field
// (service::ZeroCalDir) and is left alone -- it is relative to the encoder, not
// to the model.
//
// One row per finger, four joints each, then the reserved 21st slot, which has
// no motor behind it and stays forward.
constexpr std::array<int8_t, frames::kNumJoints> kLeftJointDirection = {
    1, 1, -1, -1,  // finger0  j0 j1 j2 j3
    1, 1, -1, -1,  // finger1
    1, 1, -1, -1,  // finger2
    1, 1, -1, -1,  // finger3
    1, 1, -1, -1,  // finger4
    1,           // reserved
};

constexpr std::array<int8_t, frames::kNumJoints> kRightJointDirection = {
    1, 1, -1, -1,  // finger0  j0 j1 j2 j3
    1, 1, -1, -1,  // finger1
    1, 1, -1, -1,  // finger2
    1, 1, -1, -1,  // finger3
    1, 1, -1, -1,  // finger4
    1,           // reserved
};

// A typo in the tables above is a build error, not something to find on a
// bench: a 0 would zero a joint's command and read its position back as zero,
// which looks exactly like a dead motor.
constexpr bool onlySigns(const std::array<int8_t, frames::kNumJoints>& dirs)
{
    for (const auto dir : dirs) {
        if (dir != 1 && dir != -1) {
            return false;
        }
    }
    return true;
}

static_assert(onlySigns(kLeftJointDirection), "kLeftJointDirection: every entry must be +1 or -1");
static_assert(onlySigns(kRightJointDirection), "kRightJointDirection: every entry must be +1 or -1");

struct JointDirections
{
    std::array<int8_t, frames::kNumJoints> left{kLeftJointDirection};
    std::array<int8_t, frames::kNumJoints> right{kRightJointDirection};
};

// EtherCAT synchronisation parameters.
//
// DC-Synchron: each ESC acts on its own drift-corrected SYNC0/SYNC1 pulses
// rather than on frame arrival. SM-Synchron: the application acts on the
// SyncManager event. FreeRun: no device sync is requested; the master still
// exchanges at sync0_cycle_ns on its own local timer. Register details and the
// reasoning behind every default here: notes.md, "DC-Synchron bring-up".
struct SyncConfig
{
    bool free_run{true};  // true = no DC/SM synchronisation requested
    bool use_dc{false};  // true = DC-Synchron, false = SM-Synchron

    // Master cycle period. In DC mode this must equal the SYNC0 period; in
    // SM/FreeRun it is simply how often the cyclic thread exchanges PDOs.
    uint32_t sync0_cycle_ns{2'000'000};

    // SYNC0 offset from the DC grid boundary.
    int32_t sync0_shift_ns{100'000};

    // SYNC1 offset from the DC grid boundary (same cycle as SYNC0).
    int32_t sync1_shift_from_sync0_ns{200'000};

    // Target phase of the master task inside the DC cycle: after SYNC0, and
    // clear of sync0_shift_ns by more than the phase jitter.
    int64_t master_offset_ns{400'000};

    // Write 0x1C32 / 0x1C33 in PREOP, asking the device for the selected sync
    // mode. In SM/DC modes this also writes the cycle/shift timing above.
    // Non-fatal: devices that hardwire these objects reject writes, and we log
    // the readback either way.
    bool write_sync_type{true};

    // Deprecated compatibility fields. The connection now derives the sync
    // type from free_run/use_dc: 0/0 FreeRun, 1/1 SM-Synchron, 2/3 DC Sync0/1.
    uint16_t out_sync_type{1};
    uint16_t in_sync_type{1};
};

struct Config
{
    std::string interface_name{"enp132s0"};
    std::string left_hand_name{"hand_200_L"};
    std::string right_hand_name{"hand_200_R"};
    SyncConfig dc{};

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

    // Which joints are fitted backwards, from the tables above.
    JointDirections joint_direction{};
};

} // namespace hand_sdk
