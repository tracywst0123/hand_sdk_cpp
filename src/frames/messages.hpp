#pragma once

#include <array>
#include <cstdint>
#include <map>

namespace hand_sdk::frames
{

constexpr std::size_t kNumJoints = 16;

enum class ControlMode : uint8_t
{
    Disabled = 0,
    Position = 1,
    Velocity = 2,
    Torque = 3,
    Impedance = 4,
};

struct ReadState
{
    std::array<float, kNumJoints> q{};
    std::array<float, kNumJoints> qd{};
    std::array<float, kNumJoints> tau{};
};

struct QCommand
{
    ControlMode mode{ControlMode::Disabled};
    std::array<float, kNumJoints> q{};
    std::array<float, kNumJoints> qd{};
    std::array<float, kNumJoints> tau{};
};

struct ParameterState
{
    std::array<float, kNumJoints> kp{};
    std::array<float, kNumJoints> kd{};
    float temperature{0.0F};
};

struct SensorMap
{
    std::map<std::string, uint16_t> sensors{};
};

} // namespace hand_sdk::frames