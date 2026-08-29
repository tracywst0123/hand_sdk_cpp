#pragma once

#include "../../include/messages.hpp"
#include "pdo_layout.hpp"

#include <cmath>
#include <cstdint>

namespace hand_sdk::frames::pdo
{

// Both headers define a joint count; they must describe the same device.
static_assert(hand_sdk::frames::kNumJoints == kNumJoints,
              "message and PDO joint counts disagree");
static_assert(hand_sdk::frames::kNumActiveJoints == kNumMotorSlots,
              "message and PDO motor counts disagree");

// Fixed-point wire encodings ("s16enN" = signed int16 with N fraction bits;
// fixed = float * 2^N):
//   position  s16en12  *4096  [-8, 8)     rad
//   velocity  s16en11  *2048  [-16, 16)   rad/s
//   torque    s16en7   *128   [-256, 256) N*m
//   loop gain s16en11  *2048  [-16, 16)   (service channel)
constexpr float kPosScale  = 4096.0f;
constexpr float kVelScale  = 2048.0f;
constexpr float kTauScale  = 128.0f;
constexpr float kGainScale = 2048.0f;

// float -> s16 fixed point for the wire.
//
// A plain static_cast of an out-of-range or non-finite float to int16_t is
// undefined behaviour, not a wrap. Api runs this on the calling thread, so
// whatever any user thread produces is guarded before the encoded frame
// enters the command holder — the cyclic thread and everything below it only
// see finished wire words. NaN becomes 0: there is no sensible value to fall
// back to, and 0 is what an unset field already holds.
inline int16_t toFixed(float v, float scale)
{
    if (std::isnan(v)) {
        return 0;
    }
    const float scaled = v * scale;
    if (scaled <= -32768.0f) {
        return INT16_MIN;
    }
    if (scaled >= 32767.0f) {
        return INT16_MAX;
    }
    return static_cast<int16_t>(std::lround(scaled));
}

// The reverse direction is exact up to float precision, so it needs no guard.
inline float fromFixed(int16_t v, float scale)
{
    return static_cast<float>(v) / scale;
}

inline void decodeJoints(const JointEntry* src,
                         std::array<float, kNumJoints>& q,
                         std::array<float, kNumJoints>& dq,
                         std::array<float, kNumJoints>& tau)
{
    // Logical joint i is read from wire slot wireSlot(i) -- the device orders
    // each finger's joints distal-first. See pdo_layout.hpp.
    for (std::size_t i = 0; i < kNumJoints; ++i) {
        const JointEntry& entry = src[wireSlot(i)];
        q[i]   = fromFixed(entry.angle, kPosScale);
        dq[i]  = fromFixed(entry.speed, kVelScale);
        tau[i] = fromFixed(entry.torque, kTauScale);
    }
}

inline void encodeJoints(const std::array<float, kNumJoints>& q,
                         const std::array<float, kNumJoints>& dq,
                         const std::array<float, kNumJoints>& tau,
                         JointEntry* dst)
{
    // Same permutation the other way round: logical joint i is written into
    // wire slot wireSlot(i).
    for (std::size_t i = 0; i < kNumJoints; ++i) {
        JointEntry& entry = dst[wireSlot(i)];
        entry.angle  = toFixed(q[i], kPosScale);
        entry.speed  = toFixed(dq[i], kVelScale);
        entry.torque = toFixed(tau[i], kTauScale);
    }
}

// Service block filled with "nothing requested": cmd 0, every payload word
// the ignore value. This is what goes out whenever no service window is open.
inline ServiceData defaultServiceData()
{
    ServiceData sd{};
    sd.cmd = 0;
    for (auto& word : sd.data) {
        word = static_cast<int16_t>(kIgnoreWord);
    }
    return sd;
}

inline HandState decodeReadPDO(const ReadPDO& pdo)
{
    HandState state{};

    decodeJoints(pdo.joints, state.q, state.dq, state.tau);

    // Error_L in the low half, Error_H in the high half; bits 0..20 = motors --
    // in *device* slot order, so the bits are permuted into logical joint order
    // like everything else. Bit 20 is the reserved slot and stays where it is.
    const uint32_t raw = static_cast<uint32_t>(pdo.error.error1) |
                         (static_cast<uint32_t>(pdo.error.error2) << 16);
    state.error_bits = raw & ~((1u << kNumMotorSlots) - 1u);
    for (std::size_t i = 0; i < kNumMotorSlots; ++i) {
        if ((raw & (1u << wireSlot(i))) != 0u) {
            state.error_bits |= 1u << i;
        }
    }

    // The service region is not decoded here: it belongs to the service
    // request/response path (service_codec.hpp + CyclicTask's service window).
    return state;
}

inline WritePDO encodeWritePDO(const HandCommand& command)
{
    WritePDO pdo{};  // value-initialised, so `extra` goes out as zeros

    encodeJoints(command.q, command.dq, command.tau, pdo.joints);

    // Placeholder only — the cyclic thread overwrites the service region from
    // its service window every cycle.
    pdo.service_data = defaultServiceData();

    return pdo;
}

// Dual-hand forms, mirroring the holder contents. These run on user threads
// (called from Api), never in the cyclic loop.
inline DualHandState decodeReadPDO(const DualReadPDO& pdo)
{
    DualHandState state{};
    if (pdo.has_left) {
        state.left     = decodeReadPDO(pdo.left);
        state.has_left = true;
    }
    if (pdo.has_right) {
        state.right     = decodeReadPDO(pdo.right);
        state.has_right = true;
    }
    return state;
}

inline DualWritePDO encodeWritePDO(const DualHandCommand& command)
{
    DualWritePDO pdo{};
    if (command.has_left) {
        pdo.left     = encodeWritePDO(command.left);
        pdo.has_left = true;
    }
    if (command.has_right) {
        pdo.right     = encodeWritePDO(command.right);
        pdo.has_right = true;
    }
    return pdo;
}

} // namespace hand_sdk::frames::pdo
