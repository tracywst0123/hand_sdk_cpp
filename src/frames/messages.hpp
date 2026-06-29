#pragma once

#include <array>

namespace hand_sdk::frames
{

constexpr std::size_t kNumJoints = 21;

struct HandState
{
    std::array<float, kNumJoints> q{};    // positions
    std::array<float, kNumJoints> dq{};   // velocities
    std::array<float, kNumJoints> tau{};  // torques
};

struct HandCommand
{
    std::array<float, kNumJoints> q{};
    std::array<float, kNumJoints> dq{};
    std::array<float, kNumJoints> tau{};
};

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