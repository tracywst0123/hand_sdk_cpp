// Minimal user-layer example.
//
//   1. connect
//   2. once a second for kRunSeconds: write a ramp into the first three
//      service_data words, then read the same three back
//   3. disconnect
//
// Only hand_sdk::Api is used here. The bus runs at 1 kHz on its own real-time
// thread the whole time; this loop just hands values in and reads them out.
//
// SAFETY: the zero-initialised q[] still ships every cycle as a position
// command of 0 on all 21 joints. Keep the hands unpowered or free to move.

#include <hand_sdk.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr int kRunSeconds = 100;      // total run length
constexpr int kRampTop    = 100;      // ramp 0 -> kRampTop -> 0 -> ...
constexpr std::size_t kSpots = 3;     // service_data.data[0..2]

std::atomic<bool> g_run{true};
void onSignal(int) { g_run = false; }

void printSpots(const char* tag, const hand_sdk::frames::HandState& s)
{
    std::cout << "  " << tag << " ";
    for (std::size_t i = 0; i < kSpots; ++i) {
        std::cout << std::setw(5) << s.service_data.data[i];
    }
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    hand_sdk::Config config;
    config.interface_name  = "enp132s0";
    config.left_hand_name  = "hand_200_L";
    config.right_hand_name = "hand_200_R";

    // --- 1. connect ---------------------------------------------------------
    auto hand = hand_sdk::createEthercat(config);

    const auto connected = hand.connect();
    if (!connected.success) {
        std::cerr << "Connect failed: " << connected.msg << "\n";
        return 1;
    }

    std::cout << "left "  << (connected.value.has_left  ? hand.leftStatus().state_text  : "absent")
              << " | right " << (connected.value.has_right ? hand.rightStatus().state_text : "absent")
              << " | DC " << (hand.dcActive() ? "armed" : "off") << "\n"
              << "running " << kRunSeconds << " s, Ctrl-C to stop early\n\n";

    // --- 2. loop ------------------------------------------------------------
    int value = 0;
    int step  = 1;

    for (int sec = 0; sec < kRunSeconds && g_run; ++sec) {
        // Ramp up to kRampTop, then back down to 0, then up again.
        value += step;
        if (value >= kRampTop) { step = -1; }
        if (value <= 0)        { step = 1; }

        // --- write ---
        hand_sdk::frames::DualHandCommand command{};
        command.has_left  = connected.value.has_left;
        command.has_right = connected.value.has_right;
        for (std::size_t i = 0; i < kSpots; ++i) {
            command.left.service_data.data[i]  = static_cast<int16_t>(value);
            command.right.service_data.data[i] = static_cast<int16_t>(value);
        }
        hand.sendCommand(command);

        std::cout << "[" << std::setw(3) << sec << "] write " << std::setw(4) << value;

        std::this_thread::sleep_for(1s);

        // --- read ---
        const auto state = hand.readState();
        if (state.success) {
            std::cout << "   read";
            if (state.value.has_left)  { printSpots("L", state.value.left); }
            if (state.value.has_right) { printSpots("R", state.value.right); }
        } else {
            std::cout << "   read failed: " << state.msg;
        }
        std::cout << std::endl;
    }

    // --- 3. disconnect ------------------------------------------------------
    const auto stats = hand.cycleStats();
    hand.disconnect();

    std::cout << "\ncycles " << stats.cycles
              << " | exchange failures " << stats.exchange_failures
              << " | phase " << stats.phase_error_ns / 1000 << " us\n"
              << "left "  << hand.leftStatus().state_text
              << " | right " << hand.rightStatus().state_text << "\n";

    return 0;
}
