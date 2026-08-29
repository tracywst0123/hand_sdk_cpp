#pragma once

#include <cstdint>

namespace hand_sdk::frames::pdo
{

// The process data is a flat array of 130 INT (16-bit) entries in each
// direction. From the device ESI (hand_200_R):
//   <Sm DefaultSize="260" StartAddress="#x1100" …>Outputs</Sm>
//   <Sm DefaultSize="260" StartAddress="#x1410" …>Inputs</Sm>
//   0x1602 / 0x1A01 mapping objects: BitSize 4176 = 130 × 32-bit map entries
//   0x7020 / 0x6010 data objects:    BitSize 2096 = 130 × INT
//
// The counts below are in *words*, matching how the ESI counts them, and are
// converted to bytes only in the static_asserts. Keep that boundary explicit —
// mixing the two units silently truncates a PDO rather than failing to build.

constexpr std::size_t kNumJoints = 21;

// --- logical joint <-> wire slot ---------------------------------------------
// The device numbers each finger's four joints the other way round from every
// layer above this one. `finger0_joint0` -- the joint nearest the palm, index 0
// everywhere in the API, in ROS and in the UI -- is the *fourth* slot the device
// reads, and the distal joint is the first. So the twenty motors go out as
// 3,2,1,0, 7,6,5,4, 11,10,9,8, ... and come back the same way.
//
// This is the only place that knows: the codecs below permute on the way to the
// wire and again on the way back, so nothing above frames/ ever sees a device
// slot number. The permutation is its own inverse, which is why encode and
// decode call the same function.
//
// The reserved 21st slot has no finger and maps to itself.
constexpr std::size_t kJointsPerFinger = 4;
constexpr std::size_t kNumMotorSlots   = kNumJoints - 1;  // 20 = 5 fingers x 4

constexpr std::size_t wireSlot(std::size_t joint)
{
    return joint < kNumMotorSlots
               ? (joint / kJointsPerFinger) * kJointsPerFinger +
                     (kJointsPerFinger - 1 - joint % kJointsPerFinger)
               : joint;
}

// The order the device expects, spelled out, so a change here has to be
// deliberate: 3,2,1,0, 7,6,5,4, ... 19,18,17,16 with the reserved slot last.
static_assert(wireSlot(0) == 3 && wireSlot(1) == 2 && wireSlot(2) == 1 &&
                  wireSlot(3) == 0,
              "finger 0 must go out distal-first");
static_assert(wireSlot(4) == 7 && wireSlot(16) == 19 && wireSlot(19) == 16,
              "every finger flips the same way");
static_assert(wireSlot(20) == 20, "the reserved slot has no finger to flip in");
static_assert(wireSlot(wireSlot(5)) == 5, "the permutation must be its own inverse");

constexpr std::size_t kNumPdoWords    = 130;

// Service payload word meaning "no action / no value here".
constexpr uint16_t kIgnoreWord = 0xFFFF;
constexpr std::size_t kJointWords     = kNumJoints * 3;  // 63: angle+speed+torque
constexpr std::size_t kServiceWords    = 1 + kJointWords;  // 64: cmd + payload
constexpr std::size_t kErrorWords      = 2;              // Error_L, Error_H

// Trailing words the device maps but we do not interpret.
constexpr std::size_t kNumCommandExtra = kNumPdoWords - kJointWords - kServiceWords;
constexpr std::size_t kNumStateExtra =
    kNumPdoWords - kJointWords - kErrorWords - kServiceWords;

#pragma pack(push, 1)

// Per-joint PDO entry: angle, speed, torque (each int16, 6 bytes total)
struct JointEntry
{
    int16_t angle;
    int16_t speed;
    int16_t torque;
};

struct ServiceData
{
    int16_t cmd;
    int16_t data[kJointWords]; 
};

struct ErrorEntry
{
    uint16_t error1;
    uint16_t error2;
};

// Master writes (commands). RxPDO 0x1602, SM2.
struct WritePDO
{
    JointEntry joints[kNumJoints];     // 63 words
    ServiceData service_data;          // 64 words
    uint16_t extra[kNumCommandExtra];  //  3 words
};

// Master reads (state). TxPDO 0x1A01, SM3.
struct ReadPDO
{
    JointEntry joints[kNumJoints];   // 63 words
    ErrorEntry error;                //  2 words
    ServiceData service_data;        // 64 words
    uint16_t extra[kNumStateExtra];  //  1 word
};

#pragma pack(pop)

// Both hands' frames plus presence flags, as one trivially-copyable value.
// This is what the CyclicTask holders carry: the cyclic thread moves these
// verbatim between holder and SyncManager buffers, and the float conversion
// happens above, in Api, on whatever thread the user called from.
//
// Not packed: only the WritePDO/ReadPDO members are wire images. The wrapper
// itself never crosses the wire, so it can keep natural alignment.
struct DualWritePDO
{
    bool has_left{false};
    WritePDO left{};
    bool has_right{false};
    WritePDO right{};
};

struct DualReadPDO
{
    bool has_left{false};
    ReadPDO left{};
    bool has_right{false};
    ReadPDO right{};
};

// These sizes bound the memcpys in EthercatConnection::copyInputs() and
// stageOutputs(), and must equal the SyncManager sizes the device advertises.
// A layout edit that changed a size would otherwise run those copies past the
// end of the process-data buffers, or leave part of the PDO unwritten.
static_assert(sizeof(JointEntry) == 6, "JointEntry must pack to 6 bytes");
static_assert(sizeof(ServiceData) == kServiceWords * 2, "ServiceData must pack to 64 words");
static_assert(sizeof(ErrorEntry) == kErrorWords * 2, "ErrorEntry must pack to 2 words");
static_assert(sizeof(WritePDO) == kNumPdoWords * 2, "WritePDO must be 260 bytes (ESI SM2 DefaultSize)");
static_assert(sizeof(ReadPDO) == kNumPdoWords * 2, "ReadPDO must be 260 bytes (ESI SM3 DefaultSize)");

} // namespace hand_sdk::frames::pdo
