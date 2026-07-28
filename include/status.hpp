#pragma once

#include <cstdint>
#include <string>

// Status types shared between the layers. Deliberately free of any SOEM or
// EtherCAT dependency, so the user-facing headers do not drag the master stack
// (and its include paths) into every consumer of this SDK.

namespace hand_sdk
{

// How the cyclic thread is doing. Each field is sampled from its own atomic, so
// they can be a cycle out of step with each other — fine for reporting, but not
// a consistent snapshot.
struct CycleStats
{
    uint64_t cycles{0};             // exchanges completed
    uint64_t exchange_failures{0};  // bad working counter (a slave did not answer)
    uint64_t command_underruns{0};  // lost the race on holder2; reused last command
    int64_t phase_error_ns{0};      // distance from the target DC phase
    bool running{false};
};

// Where a slave sits in the EtherCAT state machine.
//
// Sampled at the end of connect() and again during disconnect(), NOT while
// cycling: refreshing it means an ecx_readstate() on the wire, which would land
// inside the DC window and cost the cycle its deadline.
struct SlaveStatus
{
    bool present{false};
    std::string name{};

    // Raw AL status register. Low nibble is the state (1=INIT, 2=PRE_OP,
    // 3=BOOT, 4=SAFE_OP, 8=OP); bit 4 (0x10) is the error latch.
    uint16_t al_state{0};

    // AL status code, meaningful when the error latch is set. Values worth
    // knowing here: 0x001A sync error, 0x0030 invalid DC sync configuration,
    // 0x0032 DC PLL error, 0x001B sync manager watchdog.
    uint16_t al_status_code{0};

    std::string state_text{};  // "OP", "SAFE_OP+ERROR", ...
    std::string error_text{};  // decoded al_status_code
};

// A device's own account of whether the master met its SYNC deadlines, read
// from the CoE SyncManager parameter objects.
struct SyncDiagnostics
{
    bool valid{false};
    uint16_t out_sync_type{0};        // 0x1C32:01 — expect 0x0002 (DC SYNC0)
    uint16_t in_sync_type{0};         // 0x1C33:01 — expect 0x0003 (DC SYNC1)
    uint16_t out_cycle_exceeded{0};   // 0x1C32:0B — must not climb
    uint16_t in_cycle_exceeded{0};    // 0x1C33:0B — must not climb
    uint16_t out_shift_too_short{0};  // 0x1C32:0C
    uint16_t in_shift_too_short{0};   // 0x1C33:0C
};

} // namespace hand_sdk
