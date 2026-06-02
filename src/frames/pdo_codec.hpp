#pragma once

#include "messages.hpp"
#include "pdo_layout.hpp"

#include <algorithm>

namespace hand_sdk::frames::pdo
{

inline ReadState decodeTxPDO(const TxPDO& pdo)
{
    ReadState state{};

    for (std::size_t i = 0; i < kNumJoints; ++i) {
        state.q[i] = static_cast<float>(pdo.joint_angle[i]);
        state.qd[i] = static_cast<float>(pdo.motor_velocity[i]);
        state.tau[i] = static_cast<float>(pdo.motor_current[i]);
    }

    return state;
}

inline RxPDO encodeRxPDO(const QCommand& command)
{
    RxPDO pdo{};

    pdo.mode = static_cast<uint8_t>(command.mode);

    for (std::size_t i = 0; i < kNumJoints; ++i) {
        pdo.target_position[i] = static_cast<int16_t>(command.q[i]);
        pdo.target_velocity[i] = static_cast<int16_t>(command.qd[i]);
        pdo.target_torque[i] = static_cast<int16_t>(command.tau[i]);
    }

    return pdo;
}

} // namespace hand_sdk::frames::v1