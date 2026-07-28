#pragma once

#include <array>
#include <cstdint>

namespace hand_sdk::frames
{

constexpr std::size_t kNumJoints = 21;

// A command word plus a block of opaque service words, carried alongside the
// joint data in both PDO directions.
//
// The device ESI names these entries only OutDate1..OutDate67 (RxPDO) and
// InDate1..InDate65 (TxPDO) and documents no meaning for any of them, so they
// are passed through as raw device words rather than decoded into anything.
// Raw int16 also round-trips exactly — no clamping, no float conversion, and
// nothing lost in either direction.
//
// Until the vendor says what `cmd` selects, leaving it at 0 is the only
// defensible default; it is sent to the hands on every cycle.
struct ServiceBlock
{
    static constexpr std::size_t kNumWords = kNumJoints * 3;  // 63

    int16_t cmd{0};
    std::array<int16_t, kNumWords> data{};
};

struct HandState
{
    std::array<float, kNumJoints> q{};    // positions
    std::array<float, kNumJoints> dq{};   // velocities
    std::array<float, kNumJoints> tau{};  // torques

    // TxPDO Error_L / Error_H (0x6010:64 and :65). Raw device words.
    uint16_t error1{0};
    uint16_t error2{0};

    ServiceBlock service_data{};
};

struct HandCommand
{
    std::array<float, kNumJoints> q{};
    std::array<float, kNumJoints> dq{};
    std::array<float, kNumJoints> tau{};

    ServiceBlock service_data{};
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
