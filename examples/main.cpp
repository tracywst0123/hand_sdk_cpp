// Example of user-layer usage.
//
// This file touches nothing but hand_sdk::Api. It never sees the EtherCAT
// connection, the cyclic thread, the DC clock or the holders — the bus is
// already running at 1 kHz on its own real-time thread by the time connect()
// returns, and everything below is just handing values in and reading them out.
//
// Note there is no sleep-tuning, no cycle counting and no priority juggling
// here: user code is free to run at whatever rate it likes.
//
// What this exercises: the service-data words. Once a second for ten seconds it
// writes three distinct values into service_data.data[0..2] and reads back what
// the hands report, so you can see whether the words survive the round trip —
// while the cyclic thread keeps the bus running at 1 kHz underneath.
//
// SAFETY: not writing q/dq/tau does NOT mean no motion command is sent. Every
// cycle ships the full 260-byte PDO, so the zero-initialised q[] goes out as a
// position command of 0 on all 21 joints. Run this with the hands unpowered or
// mechanically free until you know how the device interprets that.

#include <hand_sdk.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;
using hand_sdk::frames::HandState;

// The three service words this test drives.
constexpr std::size_t kProbeWords = 3;

// Write slowly and keep the bus running for a while: at 1 Hz over 10 seconds
// the cyclic thread completes ~10000 exchanges between the first write and the
// last. That is long enough for a DC phase lock to settle and for clock drift
// or a creeping sync-error count to show itself — neither of which is visible
// in a burst that finishes in half a second.
constexpr int kIterations = 10;
constexpr auto kWriteInterval = 1000ms;

void printHand(const char* tag, const HandState& s)
{
    std::cout << "      " << tag
              << " svc.data[0..2] = " << s.service_data.data[0]
              << ", " << s.service_data.data[1]
              << ", " << s.service_data.data[2]
              << "   svc.cmd = " << s.service_data.cmd
              << "   err = 0x" << std::hex << s.error1 << "/0x" << s.error2 << std::dec
              << "   q[0] = " << std::fixed << std::setprecision(1) << s.q[0] << "\n";
}

// Did the hand hand back exactly what we wrote into the probe words?
bool echoesProbe(const HandState& s, int16_t base)
{
    for (std::size_t w = 0; w < kProbeWords; ++w) {
        if (s.service_data.data[w] != static_cast<int16_t>(base + static_cast<int16_t>(w))) {
            return false;
        }
    }
    return true;
}

void printStatus(const char* tag, const hand_sdk::SlaveStatus& s)
{
    if (!s.present) {
        std::cout << "  " << tag << ": not on the bus\n";
        return;
    }
    std::cout << "  " << tag << ": '" << s.name << "'  state " << s.state_text
              << " (0x" << std::hex << s.al_state << std::dec << ")";
    if (s.al_status_code != 0) {
        std::cout << "  AL 0x" << std::hex << s.al_status_code << std::dec
                  << " " << s.error_text;
    }
    std::cout << "\n";
}

void printDiagnostics(const char* tag, const hand_sdk::SyncDiagnostics& d)
{
    if (!d.valid) {
        std::cout << "  " << tag << ": sync diagnostics unavailable\n";
        return;
    }
    std::cout << "  " << tag << ": sync type out=0x" << std::hex << d.out_sync_type
              << " in=0x" << d.in_sync_type << std::dec
              << " | cycle exceeded out=" << d.out_cycle_exceeded
              << " in=" << d.in_cycle_exceeded
              << " | shift too short out=" << d.out_shift_too_short
              << " in=" << d.in_shift_too_short << "\n";
}

} // namespace

int main()
{
    hand_sdk::Config config;
    config.interface_name  = "enp132s0";
    config.left_hand_name  = "hand_200_L";
    config.right_hand_name = "hand_200_R";

    auto hand = hand_sdk::createEthercat(config);

    const auto connected = hand.connect();
    if (!connected.success) {
        std::cerr << "Connect failed: " << connected.msg << "\n";
        return 1;
    }

    std::cout << "Left hand:   " << (connected.value.has_left  ? "found" : "not found") << "\n"
              << "Right hand:  " << (connected.value.has_right ? "found" : "not found") << "\n"
              << "DC-Synchron: " << (hand.dcActive() ? "armed" : "off") << "\n"
              << "Bus state after connect:\n";
    printStatus("left ", hand.leftStatus());
    printStatus("right", hand.rightStatus());
    std::cout << "\n";

    // Give the cyclic thread a moment to publish its first state and, if DC is
    // armed, to phase-lock onto the slaves' clock.
    std::this_thread::sleep_for(500ms);

    int writes_accepted = 0;
    int reads_fresh     = 0;
    int echoed          = 0;

    const auto interval_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(kWriteInterval).count();

    std::cout << "Service-data write/read check: " << kIterations << " writes, one every "
              << interval_s << " s (~" << kIterations * interval_s << " s total)\n";

    for (int i = 0; i < kIterations; ++i) {
        // --- write ----------------------------------------------------------
        // Distinct per iteration AND per word, so a stale read, a duplicated
        // word and a wrong-offset read all look different from a clean echo.
        const auto base = static_cast<int16_t>(1000 * (i + 1));

        hand_sdk::frames::DualHandCommand command{};
        command.has_left  = connected.value.has_left;
        command.has_right = connected.value.has_right;
        for (std::size_t w = 0; w < kProbeWords; ++w) {
            const auto v = static_cast<int16_t>(base + static_cast<int16_t>(w));
            command.left.service_data.data[w]  = v;
            command.right.service_data.data[w] = v;
        }
        // service_data.cmd deliberately left at 0: its meaning is undocumented.

        const uint64_t cmd_seq_before   = hand.commandSequence();
        const uint64_t state_seq_before = hand.stateSequence();
        const auto stats_before         = hand.cycleStats();

        const auto written = hand.sendCommand(command);
        if (!written.success) {
            std::cout << "  [" << i << "] sendCommand failed: " << written.msg << "\n";
            continue;
        }

        // --- let the bus run --------------------------------------------------
        // The command goes out on the very next DC cycle; this wait is only so
        // the bus accumulates a meaningful number of cycles between writes.
        std::this_thread::sleep_for(kWriteInterval);

        // --- read -------------------------------------------------------------
        const auto state = hand.readState();
        const uint64_t cmd_seq_after   = hand.commandSequence();
        const uint64_t state_seq_after = hand.stateSequence();
        const auto stats_after         = hand.cycleStats();

        const bool write_landed = cmd_seq_after > cmd_seq_before;
        const bool read_fresh   = state.success && state_seq_after > state_seq_before;

        writes_accepted += write_landed ? 1 : 0;
        reads_fresh     += read_fresh ? 1 : 0;

        // Cycles completed during this interval: at a 1 ms period this should
        // read ~1000/s and stay there. A number that sags over the run means
        // the cyclic thread is losing its deadline, which the burst version of
        // this test was far too short to reveal.
        const uint64_t cycles_run = stats_after.cycles - stats_before.cycles;
        const double bus_hz = static_cast<double>(cycles_run) / interval_s;

        std::cout << "  [" << i << "] wrote svc.data[0..2] = "
                  << base << ", " << base + 1 << ", " << base + 2
                  << " -> cmd seq " << cmd_seq_before << "->" << cmd_seq_after
                  << (write_landed ? " (ok)" : " (NOT STORED)")
                  << " | state seq " << state_seq_before << "->" << state_seq_after
                  << (read_fresh ? " (fresh)" : " (STALE)") << "\n"
                  << "      bus: " << cycles_run << " cycles (" << std::fixed
                  << std::setprecision(0) << bus_hz << " Hz)"
                  << "  fails " << stats_after.exchange_failures
                  << "  underruns " << stats_after.command_underruns
                  << "  phase " << stats_after.phase_error_ns / 1000 << " us\n";

        if (state.success) {
            bool any_echo = false;
            if (state.value.has_left) {
                printHand("left ", state.value.left);
                any_echo = any_echo || echoesProbe(state.value.left, base);
            }
            if (state.value.has_right) {
                printHand("right", state.value.right);
                any_echo = any_echo || echoesProbe(state.value.right, base);
            }
            if (any_echo) {
                ++echoed;
                std::cout << "      -> ECHO: a hand returned exactly what we wrote\n";
            }
        } else {
            std::cout << "      readState failed: " << state.msg << "\n";
        }
    }

    // --- verdict ------------------------------------------------------------
    const auto stats = hand.cycleStats();

    std::cout << "\nResult:\n"
              << "  writes stored in holder2:   " << writes_accepted << "/" << kIterations << "\n"
              << "  reads fresh from holder1:   " << reads_fresh << "/" << kIterations << "\n"
              << "  bus cycles run:             " << stats.cycles << "\n"
              << "  exchange failures:          " << stats.exchange_failures << "\n"
              << "  command underruns:          " << stats.command_underruns << "\n"
              << "  DC phase error:             " << stats.phase_error_ns / 1000 << " us\n"
              << "  service words echoed back:  " << echoed << "/" << kIterations
              << "  (informational)\n";

    // Echo is NOT part of the verdict: nothing documents whether these words are
    // echoed at all, so "no echo" may simply mean the device ignores them. The
    // verdict covers only what this SDK is responsible for — that both
    // directions of the data path are live and moving.
    const bool pass = writes_accepted == kIterations && reads_fresh == kIterations &&
                      stats.cycles > 0 && stats.exchange_failures == 0;
    std::cout << "  verdict:                    " << (pass ? "PASS" : "FAIL") << "\n";

    const bool was_dc = hand.dcActive();
    hand.disconnect();

    // Captured as the bus was torn down: if a hand fell out of OP mid-run —
    // sync error, watchdog, cable — this is where it shows.
    std::cout << "\nBus state after run:\n";
    printStatus("left ", hand.leftStatus());
    printStatus("right", hand.rightStatus());

    if (was_dc || hand.leftDiagnostics().valid || hand.rightDiagnostics().valid) {
        std::cout << "\nDevice sync diagnostics:\n";
        printDiagnostics("left ", hand.leftDiagnostics());
        printDiagnostics("right", hand.rightDiagnostics());
    }

    return pass ? 0 : 1;
}
