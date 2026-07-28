#pragma once

#include "../../include/messages.hpp"
#include "pdo_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace hand_sdk::frames::pdo
{

// Both headers define a joint count; they must describe the same device.
static_assert(hand_sdk::frames::kNumJoints == kNumJoints,
              "message and PDO joint counts disagree");
static_assert(sizeof(hand_sdk::frames::ServiceBlock::data) == sizeof(ServiceData::data),
              "message and PDO service payloads disagree");


// float -> int16 for the wire.
//
// A plain static_cast of an out-of-range or non-finite float to int16_t is
// undefined behaviour, not a wrap. Commands reach here from holder2, which
// accepts whatever any thread writes into it, and the cast then executes inside
// the real-time cycle — so the guard belongs here rather than in a convention
// callers are trusted to follow. NaN becomes 0: there is no sensible joint
// value to fall back to, and 0 is what an unset field already holds.
inline int16_t toWire(float v)
{
    if (std::isnan(v)) {
        return 0;
    }
    if (v <= -32768.0f) {
        return INT16_MIN;
    }
    if (v >= 32767.0f) {
        return INT16_MAX;
    }
    return static_cast<int16_t>(v);
}

// int16 -> float is always exact (every int16 is representable), so the reverse
// direction needs no guard.
inline void decodeJoints(const JointEntry* src,
                         std::array<float, kNumJoints>& q,
                         std::array<float, kNumJoints>& dq,
                         std::array<float, kNumJoints>& tau)
{
    for (std::size_t i = 0; i < kNumJoints; ++i) {
        q[i]   = static_cast<float>(src[i].angle);
        dq[i]  = static_cast<float>(src[i].speed);
        tau[i] = static_cast<float>(src[i].torque);
    }
}

inline void encodeJoints(const std::array<float, kNumJoints>& q,
                         const std::array<float, kNumJoints>& dq,
                         const std::array<float, kNumJoints>& tau,
                         JointEntry* dst)
{
    for (std::size_t i = 0; i < kNumJoints; ++i) {
        dst[i].angle  = toWire(q[i]);
        dst[i].speed  = toWire(dq[i]);
        dst[i].torque = toWire(tau[i]);
    }
}

inline HandState decodeReadPDO(const ReadPDO& pdo)
{
    HandState state{};

    decodeJoints(pdo.joints, state.q, state.dq, state.tau);

    state.error1 = pdo.error.error1;
    state.error2 = pdo.error.error2;

    // Opaque: copied verbatim, not interpreted or converted.
    state.service_data.cmd = pdo.service_data.cmd;
    std::copy(std::begin(pdo.service_data.data), std::end(pdo.service_data.data),
              state.service_data.data.begin());

    // pdo.extra is left unread: undocumented trailing words.
    return state;
}

inline WritePDO encodeWritePDO(const HandCommand& command)
{
    WritePDO pdo{};  // value-initialised, so `extra` goes out as zeros

    encodeJoints(command.q, command.dq, command.tau, pdo.joints);

    // Opaque: copied verbatim, so no clamping is needed or wanted here.
    pdo.service_data.cmd = command.service_data.cmd;
    std::copy(command.service_data.data.begin(), command.service_data.data.end(),
              std::begin(pdo.service_data.data));

    return pdo;
}

} // namespace hand_sdk::frames::pdo
