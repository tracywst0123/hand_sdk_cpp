#pragma once

#include "messages.hpp"
#include "pdo_layout.hpp"

namespace hand_sdk::frames::pdo
{

inline HandState decodeReadPDO(const ReadPDO& pdo)
{
    HandState state{};

    for (std::size_t i = 0; i < kNumJoints; ++i) {
        state.q[i]   = static_cast<float>(pdo.joints[i].angle);
        state.dq[i]  = static_cast<float>(pdo.joints[i].speed);
        state.tau[i] = static_cast<float>(pdo.joints[i].torque);
    }

    return state;
}

inline WritePDO encodeWritePDO(const HandCommand& command)
{
    WritePDO pdo{};

    for (std::size_t i = 0; i < kNumJoints; ++i) {
        pdo.joints[i].angle  = static_cast<int16_t>(command.q[i]);
        pdo.joints[i].speed  = static_cast<int16_t>(command.dq[i]);
        pdo.joints[i].torque = static_cast<int16_t>(command.tau[i]);
    }

    return pdo;
}

} // namespace hand_sdk::frames::pdo