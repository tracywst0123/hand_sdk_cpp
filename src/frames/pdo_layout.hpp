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

constexpr std::size_t kNumPdoWords    = 130;
constexpr std::size_t kJointWords     = kNumJoints * 3;  // 63: angle+speed+torque
constexpr std::size_t kServiceDataWords = kNumJoints * 3; // 63: opaque, see ServiceData
constexpr std::size_t kServiceWords    = 1 + kServiceDataWords;  // 64: cmd + payload
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

// A command word followed by 63 further words. These are not joint triples —
// the ESI names them only OutDate1..67 / InDate1..65 and documents no meaning
// for any of them, so they are carried opaquely rather than interpreted.
struct ServiceData
{
    int16_t cmd;
    int16_t data[kServiceDataWords];
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

// These sizes bound the memcpys in EthercatConnection::readState() and
// stageCommand(), and must equal the SyncManager sizes the device advertises.
// A layout edit that changed a size would otherwise run those copies past the
// end of the process-data buffers, or leave part of the PDO unwritten.
static_assert(sizeof(JointEntry) == 6, "JointEntry must pack to 6 bytes");
static_assert(sizeof(ServiceData) == kServiceWords * 2, "ServiceData must pack to 64 words");
static_assert(sizeof(ErrorEntry) == kErrorWords * 2, "ErrorEntry must pack to 2 words");
static_assert(sizeof(WritePDO) == kNumPdoWords * 2, "WritePDO must be 260 bytes (ESI SM2 DefaultSize)");
static_assert(sizeof(ReadPDO) == kNumPdoWords * 2, "ReadPDO must be 260 bytes (ESI SM3 DefaultSize)");

} // namespace hand_sdk::frames::pdo
