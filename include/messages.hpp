#pragma once

#include <array>
#include <cstdint>

namespace hand_sdk::frames
{

constexpr std::size_t kNumJoints = 21;

// Engineering units: q in rad, dq in rad/s, tau in N*m. The wire carries
// fixed-point int16 words; Api converts on the calling thread.
struct HandState
{
    std::array<float, kNumJoints> q{};    // positions  [rad]
    std::array<float, kNumJoints> dq{};   // velocities [rad/s]
    std::array<float, kNumJoints> tau{};  // torques    [N*m]

    // Per-motor fault bits from TxPDO Error_L/Error_H: bit i = motor i,
    // 1 = fault, 0 = normal. Bits 21..31 unused.
    uint32_t error_bits{0};
};

struct HandCommand
{
    std::array<float, kNumJoints> q{};
    std::array<float, kNumJoints> dq{};
    std::array<float, kNumJoints> tau{};
};

// The PDO service channel (cmd + 3 words per joint) is not part of these
// message types: it is owned by the Api service calls (include/service.hpp).

struct DualHandState
{
    bool has_left{false};
    HandState left{};
    bool has_right{false};
    HandState right{};
};

struct DualHandCommand
{
    bool has_left{false};
    HandCommand left{};
    bool has_right{false};
    HandCommand right{};
};

} // namespace hand_sdk::frames
