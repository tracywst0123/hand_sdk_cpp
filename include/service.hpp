#pragma once

#include "messages.hpp"

#include <array>
#include <cmath>
#include <cstdint>

// Typed vocabulary for the PDO service channel. Each Api::service*() call is
// one request/response round-trip: the request is held on the bus for a
// 10-cycle window and the device's reply is decoded into these types
// (protocol details: notes.md, "Service channel").

namespace hand_sdk::service
{

// Which hand a service call targets.
enum class Hand { kLeft, kRight };

// Which joints a service call targets.
//
// Addressing a subset is native to the protocol rather than something layered
// on top: the payload is 3 words per joint and 0xFFFF means "no action", so a
// masked request carries real values for the selected joints and the ignore
// word for the rest. One motor can be re-tuned without disturbing the other
// nineteen.
//
// Default-constructed is EMPTY. The service calls default to all(), which is
// the broadcast they have always done.
class JointMask
{
public:
    static constexpr std::size_t kSize = frames::kNumJoints;

    JointMask() = default;
    explicit JointMask(uint32_t bits) : bits_(bits & kAllBits) {}
    JointMask(std::initializer_list<std::size_t> joints)
    {
        for (const auto joint : joints) {
            set(joint);
        }
    }

    // Every slot the frame has, reserved one included -- what the service calls
    // have always broadcast to, and what they still put on the wire.
    static JointMask all() { return JointMask(kAllBits); }

    // Every slot with a motor behind it. The write services verify against this
    // rather than all(): the reserved 21st slot is written like the others but
    // has nothing there to echo, so holding it to an answer would fail every
    // call for a joint that does not exist.
    static JointMask active() { return JointMask(kActiveBits); }

    static JointMask none() { return JointMask(); }
    static JointMask single(std::size_t joint) { return JointMask{joint}; }

    JointMask& set(std::size_t joint, bool on = true)
    {
        if (joint < kSize) {
            const uint32_t bit = 1u << joint;
            bits_ = on ? (bits_ | bit) : (bits_ & ~bit);
        }
        return *this;
    }

    bool test(std::size_t joint) const
    {
        return joint < kSize && (bits_ & (1u << joint)) != 0;
    }

    bool any() const { return bits_ != 0; }
    bool isAll() const { return bits_ == kAllBits; }

    std::size_t count() const
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i < kSize; ++i) {
            n += test(i) ? 1 : 0;
        }
        return n;
    }

    uint32_t bits() const { return bits_; }

    JointMask& operator|=(const JointMask& other)
    {
        bits_ |= other.bits_;
        return *this;
    }

    JointMask& operator&=(const JointMask& other)
    {
        bits_ &= other.bits_;
        return *this;
    }

private:
    static constexpr uint32_t kAllBits = (1u << kSize) - 1u;
    static constexpr uint32_t kActiveBits = (1u << frames::kNumActiveJoints) - 1u;
    uint32_t bits_{0};
};

// --- cmd_id 4: communication control ----------------------------------------
// The request is stacked into data0 of every joint slot, like every other
// service. The reply is checked the same way the others are: each joint must
// echo the command it was given.
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

using ComStatusArray = std::array<uint16_t, frames::kNumJoints>;


inline bool took(uint16_t got, ComCmd cmd)
{
    return got == static_cast<uint16_t>(cmd);
}

inline bool took(const ModeStatus& got, LoopMode mode, bool enable)
{
    return got.mode == mode && got.enabled == enable;
}

// Gains cross a float-to-fixed-point boundary (scale 2048), so this one is a
// tolerance rather than equality: one step of 1/2048 is the conversion, more is
// the device choosing its own value. Pass Config::service_tolerance.
inline bool took(const JointGains& got, float kp, float kd, float tolerance)
{
    return std::fabs(got.kp - kp) <= tolerance && std::fabs(got.kd - kd) <= tolerance;
}

} // namespace hand_sdk::service
