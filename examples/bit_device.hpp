#pragma once

// Generic bit-IO helpers for debugging an EtherCAT slave whose process data
// is just a handful of input/output bits (e.g. the SSC-IO sample device).
//
// These helpers are intentionally layout-agnostic: they read raw bits straight
// out of the SOEM IOmap pointers so you can verify the SOEM bring-up pipeline
// (init -> config_map -> OP -> process data) in isolation from any
// device-specific PDO decoding.

#include <cstddef>
#include <cstdint>
#include <string>

namespace bit_device
{

// Read a single bit out of a byte buffer.
//
// `base`       : pointer to the first IOmap byte for this slave's data.
// `start_bit`  : SOEM's Istartbit/Ostartbit (bit offset within the first byte).
// `bit_index`  : logical channel index (0-based), counted from start_bit.
inline bool readBit(const uint8_t* base, std::size_t start_bit, std::size_t bit_index)
{
    if (base == nullptr) {
        return false;
    }
    const std::size_t abs_bit = start_bit + bit_index;
    const std::size_t byte    = abs_bit / 8;
    const std::size_t bit     = abs_bit % 8;
    return (base[byte] >> bit) & 0x1u;
}

// Write a single bit into a byte buffer (e.g. slave.outputs).
//
// `base`       : pointer to the first IOmap byte for this slave's output data.
// `start_bit`  : SOEM's Ostartbit (bit offset within the first byte).
// `bit_index`  : logical channel index (0-based), counted from start_bit.
inline void writeBit(uint8_t* base, std::size_t start_bit, std::size_t bit_index, bool value)
{
    if (base == nullptr) {
        return;
    }
    const std::size_t abs_bit = start_bit + bit_index;
    const std::size_t byte    = abs_bit / 8;
    const std::size_t bit     = abs_bit % 8;
    if (value) {
        base[byte] |= static_cast<uint8_t>(1u << bit);
    } else {
        base[byte] &= static_cast<uint8_t>(~(1u << bit));
    }
}

// Number of whole bytes spanned by `num_bits` starting at `start_bit`.
inline std::size_t byteSpan(std::size_t start_bit, std::size_t num_bits)
{
    if (num_bits == 0) {
        return 0;
    }
    return (start_bit + num_bits + 7) / 8;
}

// Render `num_bits` channels as "b7 b6 ... b1 b0" (MSB-first within each byte
// group is not used; we print channel index order, channel 0 on the right).
inline std::string formatBits(const uint8_t* base, std::size_t start_bit, std::size_t num_bits)
{
    std::string out;
    out.reserve(num_bits * 2);
    // Print high channel index first so it reads like a binary number with
    // channel 0 as the least-significant bit on the right.
    for (std::size_t i = num_bits; i-- > 0;) {
        out += readBit(base, start_bit, i) ? '1' : '0';
        if (i != 0 && (i % 4) == 0) {
            out += ' ';
        }
    }
    return out;
}

// Render raw bytes as space-separated hex, e.g. "0x03 0xA0".
inline std::string formatHex(const uint8_t* base, std::size_t num_bytes)
{
    static const char* kHex = "0123456789ABCDEF";
    if (base == nullptr || num_bytes == 0) {
        return "(none)";
    }
    std::string out;
    out.reserve(num_bytes * 5);
    for (std::size_t i = 0; i < num_bytes; ++i) {
        out += "0x";
        out += kHex[(base[i] >> 4) & 0xF];
        out += kHex[base[i] & 0xF];
        if (i + 1 < num_bytes) {
            out += ' ';
        }
    }
    return out;
}

} // namespace bit_device
