#pragma once

#include <cstdint>

namespace hand_sdk::frames::pdo
{

#pragma pack(push, 1)

struct TxPDO
{
    int16_t joint_angle[16];
    int16_t motor_velocity[16];
    int16_t motor_current[16];
};

struct RxPDO
{
    uint8_t mode;
    int16_t target_position[16];
    int16_t target_velocity[16];
    int16_t target_torque[16];
};

#pragma pack(pop)

} // namespace hand_sdk::frames::v1