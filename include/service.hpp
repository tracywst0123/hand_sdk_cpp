#pragma once

#include "messages.hpp"

#include <array>
#include <cstdint>

// Typed vocabulary for the PDO service channel. Each Api::service*() call is
// one request/response round-trip: the request is held on the bus for a
// 10-cycle window and the device's reply is decoded into these types
// (protocol details: notes.md, "Service channel").

namespace hand_sdk::service
{

// Which hand a service call targets.
enum class Hand { kLeft, kRight };

// --- cmd_id 4: communication control ----------------------------------------
// Write-only; the device sends no meaningful reply, so the call reports
// success once the request has been held for the full service window.
enum class ComCmd : uint16_t
{
    kStopBroadcast  = 1,
    kStartBroadcast = 2,
};

// --- cmd_id 11: closed-loop mode + control enable ----------------------------
enum class LoopMode : uint8_t
{
    kForceControl = 0x00,
    kThreeLoop    = 0x01,  // three-loop cascade, for debugging
};

struct ModeStatus
{
    LoopMode mode{LoopMode::kForceControl};
    bool enabled{false};  // true: electric control enabled (standby -> active)
};

// --- cmd_id 12: motor-control state + fault code (read-only) -----------------
enum class McState : uint8_t
{
    kNone    = 0,
    kInit    = 1,
    kStandby = 2,
    kActive  = 3,
    kFault   = 4,
};

// Bit flags in McStatus::fault_code.
namespace fault
{
constexpr uint32_t kNone         = 0;
constexpr uint32_t kOverCurrent  = 1u << 0;
constexpr uint32_t kOverVoltage  = 1u << 1;
constexpr uint32_t kUnderVoltage = 1u << 2;
constexpr uint32_t kStall        = 1u << 3;
constexpr uint32_t kOverTemp     = 1u << 4;
constexpr uint32_t kCurrentZero  = 1u << 5;  // current zero-offset error
constexpr uint32_t kPosNotSync   = 1u << 6;  // position out of sync (失位)
constexpr uint32_t kOverSpeed    = 1u << 7;
constexpr uint32_t kHardware     = 1u << 8;
} // namespace fault

struct McStatus
{
    McState state{McState::kInit};
    uint32_t fault_code{fault::kNone};
};

// --- cmd_id 14: force-mode position loop gains -------------------------------
struct JointGains
{
    float kp{0.0f};  // N*m/rad
    float kd{0.0f};  // N*m*s/rad (equivalent to the speed-loop kp)
};

// Replies are per joint: the service payload carries 3 words per joint.
using ModeStatusArray = std::array<ModeStatus, frames::kNumJoints>;
using McStatusArray   = std::array<McStatus, frames::kNumJoints>;
using JointGainsArray = std::array<JointGains, frames::kNumJoints>;

} // namespace hand_sdk::service
