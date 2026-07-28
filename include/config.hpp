#pragma once

#include <cstdint>
#include <string>

namespace hand_sdk
{

// Distributed Clocks (DC-Synchron) parameters.
//
// The hands' ESI declares DC-Synchron as an operating mode: each ESC generates a
// SYNC0 hardware pulse from its own drift-corrected clock and acts on that
// instant rather than on frame arrival — applying the command PDO to the motors
// and latching sensors into the state PDO.
//
// SYNC0 only. The ESI is explicit that SYNC1 is not available on this device:
//   <AssignActivate>#x300</AssignActivate>   -> 0x0981 = 0x03 = cyclic + SYNC0
//                                               (SYNC0+SYNC1 would be #x700)
//   0x1C32:04 / 0x1C33:04 = 0x8007           -> bit 3 (DC SYNC1) clear
// So this uses ecx_dcsync0(), which writes exactly the 0x03 the ESI asks for.
struct DcConfig
{
    // Set false to leave the ESCs in SM-Synchron / FreeRun (no SYNC pulses).
    bool enabled{true};

    // SYNC0 period. Must equal the master cycle period.
    // Floor is the device's 0x1C32:05 Minimum Cycle Time, 100 us.
    uint32_t sync0_cycle_ns{1'000'000};

    // SYNC0 offset from the DC grid boundary. The TwinCAT dialog composes this
    // as user_defined(-400 us) + sync0_cycle/divider(1000/2 = 500 us) = +100 us.
    int32_t sync0_shift_ns{100'000};

    // Where inside the DC cycle the master task should run.
    //
    // Comfortably *after* SYNC0 (+100 us), which is what keeps this cheap: the
    // state we read was latched at the last SYNC0, and the command we write is
    // picked up by the next one with ~700 us of margin. Squeezing the exchange
    // into the 100 us before SYNC0 instead would demand a single-frame rewrite
    // of exchangeRaw().
    //
    // Keep it clear of sync0_shift_ns by more than the cycle's phase jitter, or
    // the exchange straddles the pulse and the hands see a half-updated PDO.
    // Lowering it toward +200 us buys fresher state at the cost of that guard.
    int64_t master_offset_ns{400'000};

    // Tell the device to *use* DC mode, by writing 0x1C32:01 / 0x1C33:01 in
    // PREOP. Arming the ESC pulses is not always enough on its own: the CoE
    // application also has to be told to act on them rather than on SyncManager
    // events. TwinCAT issues these writes from the ESI's DC startup commands,
    // and they do not survive a power cycle, so we reissue them here.
    //
    // Non-fatal: devices that hardwire the sync type reject the write, and we
    // just log what they report instead. Set false to skip entirely.
    bool write_sync_type{true};

    // 0 = FreeRun, 1 = SM-Synchron, 2 = DC SYNC0, 3 = DC SYNC1.
    // Both directions key off SYNC0 — the device does not offer SYNC1, and its
    // ESI defaults are 0x1C32:01 = 1 (SM-Synchron) and 0x1C33:01 = 0x22
    // (synchron with the SM2 event), neither of which is DC.
    uint16_t out_sync_type{2};
    uint16_t in_sync_type{2};
};

struct Config
{
    std::string interface_name{"enp132s0"};
    std::string left_hand_name{"hand_200_L"};
    std::string right_hand_name{"hand_200_R"};
    DcConfig dc{};

    // SCHED_FIFO priority for the cyclic thread. Applied to that thread only —
    // user threads stay at normal priority and remain preemptible by it.
    int rt_priority{80};
};

} // namespace hand_sdk
