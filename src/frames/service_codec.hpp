#pragma once

#include "../../include/service.hpp"
#include "pdo_codec.hpp"

#include <cstdint>

// Encode typed service requests into wire ServiceData blocks and decode the
// device's replies. Runs on user threads (called from Api), never in the
// cyclic loop. The payload is 3 words per joint, data[joint*3 + 0..2]; a write
// carries its values in the slots of the joints it targets and the ignore word
// everywhere else, so a request can address one motor or all of them. Replies
// decode per joint.

namespace hand_sdk::frames::pdo
{

constexpr int16_t kIgnore = static_cast<int16_t>(kIgnoreWord);

inline void fillSlots(ServiceData& sd, int16_t d0, int16_t d1, int16_t d2)
{
    for (std::size_t j = 0; j < kNumJoints; ++j) {
        sd.data[j * 3 + 0] = d0;
        sd.data[j * 3 + 1] = d1;
        sd.data[j * 3 + 2] = d2;
    }
}

// Selected joints get the values, the rest get the ignore word -- which is the
// device's own "this slot carries no action", so unselected motors keep
// whatever they were set to.
inline void fillSelected(ServiceData& sd, const service::JointMask& joints,
                         int16_t d0, int16_t d1, int16_t d2)
{
    for (std::size_t j = 0; j < kNumJoints; ++j) {
        const bool on = joints.test(j);
        sd.data[j * 3 + 0] = on ? d0 : kIgnore;
        sd.data[j * 3 + 1] = on ? d1 : kIgnore;
        sd.data[j * 3 + 2] = on ? d2 : kIgnore;
    }
}

// The service word carries the direction in bit 15 (1 = write, 0 = read) and
// the cmd id in bits 14:8, in both directions; the low byte is reserved.
inline int16_t cmdWord(uint8_t id, bool write)
{
    const auto high = static_cast<uint16_t>((write ? 0x80u : 0x00u) | (id & 0x7Fu));
    return static_cast<int16_t>(high << 8);
}

inline uint8_t cmdId(int16_t cmd)
{
    return static_cast<uint8_t>((static_cast<uint16_t>(cmd) >> 8) & 0x7Fu);
}

inline bool cmdIsWrite(int16_t cmd)
{
    return (static_cast<uint16_t>(cmd) & 0x8000u) != 0;
}

inline uint16_t word(const ServiceData& sd, std::size_t joint, std::size_t pos)
{
    return static_cast<uint16_t>(sd.data[joint * 3 + pos]);
}

// --- cmd_id 4: communication control (write-only) ---------------------------
inline ServiceData encodeComm(service::ComCmd cmd)
{
    ServiceData sd{};
    sd.cmd = cmdWord(4, true);
    fillSlots(sd, static_cast<int16_t>(cmd), kIgnore, kIgnore);
    return sd;
}

// --- cmd_id 11: loop mode + control enable -----------------------------------
inline ServiceData encodeMode(service::LoopMode mode, bool enable,
                              const service::JointMask& joints)
{
    ServiceData sd{};
    sd.cmd = cmdWord(11, true);
    const auto mode_word = static_cast<int16_t>(
        static_cast<uint16_t>(mode) | (enable ? 1u << 8 : 0u));
    fillSelected(sd, joints, mode_word, kIgnore, kIgnore);
    return sd;
}

inline bool decodeMode(const ServiceData& sd, service::ModeStatusArray& out)
{
    if (cmdId(sd.cmd) != 11) {
        return false;
    }
    for (std::size_t j = 0; j < kNumJoints; ++j) {
        const uint16_t w = word(sd, j, 0);
        out[j].mode    = static_cast<service::LoopMode>(w & 0xFF);
        out[j].enabled = ((w >> 8) & 1u) != 0;
    }
    return true;
}

// --- cmd_id 12: mc state + fault code (read-only) ----------------------------
// Read-only query: payload words go out as zeros, not ignores.
inline ServiceData encodeMcQuery()
{
    ServiceData sd{};
    sd.cmd = cmdWord(12, false);  // false is read-only encoding
    return sd;
}

inline bool decodeMc(const ServiceData& sd, service::McStatusArray& out)
{
    if (cmdId(sd.cmd) != 12) {
        return false;
    }
    for (std::size_t j = 0; j < kNumJoints; ++j) {
        out[j].state = static_cast<service::McState>(word(sd, j, 0) & 0xFF);
        out[j].fault_code = static_cast<uint32_t>(word(sd, j, 1)) |
                            (static_cast<uint32_t>(word(sd, j, 2)) << 16);
    }
    return true;
}

// --- cmd_id 14: force-mode position loop gains -------------------------------
inline ServiceData encodeGains(float kp, float kd, const service::JointMask& joints)
{
    ServiceData sd{};
    sd.cmd = cmdWord(14, true);
    fillSelected(sd, joints, toFixed(kp, kGainScale), toFixed(kd, kGainScale), kIgnore);
    return sd;
}

inline bool decodeGains(const ServiceData& sd, service::JointGainsArray& out)
{
    if (cmdId(sd.cmd) != 14) {
        return false;
    }
    for (std::size_t j = 0; j < kNumJoints; ++j) {
        out[j].kp = fromFixed(sd.data[j * 3 + 0], kGainScale);
        out[j].kd = fromFixed(sd.data[j * 3 + 1], kGainScale);
    }
    return true;
}

} // namespace hand_sdk::frames::pdo
