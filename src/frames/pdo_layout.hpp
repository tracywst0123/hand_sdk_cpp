#pragma once

#include <cstdint>

namespace hand_sdk::frames::pdo
{

#pragma pack(push, 1)

constexpr std::size_t kNumJoints       = 21;
constexpr std::size_t kNumCommandExtra = 27; // trailing int16 words in command PDO
constexpr std::size_t kNumStateExtra   = 12; // trailing int16 words in state PDO

// Per-joint PDO entry: angle, speed, torque (each int16, 6 bytes total)
struct JointEntry
{
    int16_t angle;
    int16_t speed;
    int16_t torque;
};

// WritePDO: master writes (commands). SOEM slave.outputs, Obytes = 180.
// 21*(angle,speed,torque)*2 + 27*2 = 126 + 54 = 180 bytes.
struct WritePDO
{
    JointEntry joints[kNumJoints];      // 21 joints
    int16_t    extra[kNumCommandExtra]; // 27 trailing words
};

// ReadPDO: master reads (state). SOEM slave.inputs, Ibytes = 150.
// 21*(angle,speed,torque)*2 + 12*2 = 126 + 24 = 150 bytes.
struct ReadPDO
{
    JointEntry joints[kNumJoints];    // 21 joints
    int16_t    extra[kNumStateExtra]; // 12 trailing words
};

static_assert(sizeof(WritePDO) == 180, "WritePDO must be 180 bytes (master outputs / Obytes)");
static_assert(sizeof(ReadPDO)  == 150, "ReadPDO must be 150 bytes (master inputs / Ibytes)");

#pragma pack(pop)

} // namespace hand_sdk::frames::pdo