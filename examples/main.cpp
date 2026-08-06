// Minimal user-layer example.
//
//   1. connect
//   2. bring-up, one service call per second: start broadcast, enable
//      force-control, verify the state machine, set gains (3.0, 3.0)
//   3. once a second: write a random q/dq/tau setpoint to joint 0 and read
//      that joint's q/dq/tau back, plus the per-motor error bits
//   4. disconnect
//
// Only hand_sdk::Api is used here. The bus runs at 1 kHz on its own real-time
// thread the whole time; this loop just reads states and makes an occasional
// service round-trip.
//
// SAFETY: step 2 ENABLES CONTROL. Joints 1..20 are streamed zeros and will
// actively hold position 0 with the gains set below; joint 0 additionally gets
// a new random setpoint every second. Keep the hands free to move.

#include <hand_sdk.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>

namespace {

using namespace std::chrono_literals;
using hand_sdk::service::Hand;

constexpr auto kRunTime = 15h;

// Deliberately small: joint 0 is driven to these setpoints for real.
constexpr float kRandQ   = 0.20f;  // [rad]
constexpr float kRandDq  = 0.50f;  // [rad/s]
constexpr float kRandTau = 0.05f;  // [N*m]

std::atomic<bool> g_run{true};
void onSignal(int) { g_run = false; }

void printState(const char* tag, const hand_sdk::frames::HandState& s)
{
    std::cout << "  " << tag << " joint1 q=" << std::fixed << std::setprecision(3)
              << s.q[1] << " dq=" << s.dq[1] << " tau=" << s.tau[1]
              << "  errors=0x" << std::hex << std::setw(8) << std::setfill('0') << s.error_bits
              << " (Error_L 0x" << std::setw(4) << (s.error_bits & 0xFFFFu)
              << " Error_H 0x" << std::setw(4) << (s.error_bits >> 16) << ")"
              << std::dec << std::setfill(' ');
}

// Bring-up sequence for one hand, pausing 1 s after each service call:
// start broadcast -> force-control + enable -> verify mc_state -> set gains.
// Returns false on the first failed step.
bool bringUp(hand_sdk::Api& api, const char* tag, Hand hand)
{
    using hand_sdk::service::ComCmd;
    using hand_sdk::service::LoopMode;
    using hand_sdk::service::McState;

    // 1. start responding to broadcast (power-on default is off).
    const auto comm = api.serviceComm(hand, ComCmd::kStartBroadcast);
    std::cout << "  " << tag << " comm start broadcast: "
              << (comm.msg) << "\n";
            //   << (comm.success ? "ok" : comm.msg) << "\n";
    if (!comm.success) {
        return false;
    }
    std::this_thread::sleep_for(1s);

    // 2. force-control mode with control enabled (standby -> active).
    const auto mode = api.serviceMode(hand, LoopMode::kForceControl, true);
    if (!mode.success) {
        std::cout << "  " << tag << " mode set failed: " << mode.msg << "\n";
        return false;
    }
    std::cout << "  " << tag << " mode set: joint0 reports mode="
              << static_cast<int>(mode.value[0].mode)
              << " enabled=" << mode.value[0].enabled << "\n";
    std::this_thread::sleep_for(1s);

    // 3. every joint should now be in standby or active.
    const auto mc = api.serviceMcState(hand);
    if (!mc.success) {
        std::cout << "  " << tag << " mc-state query failed: " << mc.msg << "\n";
        return false;
    }
    int not_ready = 0;
    for (const auto& joint : mc.value) {
        if (joint.state != McState::kStandby && joint.state != McState::kActive) {
            ++not_ready;
        }
    }
    std::cout << "  " << tag << " mc-state: "
              << (21 - not_ready) << "/21 joints standby|active\n";
    if (not_ready > 0) {
        return false;
    }
    std::this_thread::sleep_for(1s);

    // 4. force-mode position loop gains, broadcast to all joints.
    const auto gains = api.serviceGains(hand, 3.0f, 3.0f);
    if (!gains.success) {
        std::cout << "  " << tag << " gains set failed: " << gains.msg << "\n";
        return false;
    }
    std::cout << "  " << tag << " gains set: joint0 reports kp="
              << gains.value[0].kp << " kd=" << gains.value[0].kd << "\n";
    std::this_thread::sleep_for(1s);

    return true;
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
              << "running " << std::chrono::duration_cast<std::chrono::hours>(kRunTime).count()
              << " h, Ctrl-C to stop early\n\n";

    // --- 2. bring-up, one service call per second ----------------------------
    std::this_thread::sleep_for(1s);
    if (connected.value.has_left && !bringUp(hand, "L", Hand::kLeft)) {
        std::cout << "  L bring-up aborted\n";
    }
    std::this_thread::sleep_for(1s);
    if (connected.value.has_right && !bringUp(hand, "R", Hand::kRight)) {
        std::cout << "  R bring-up aborted\n";
    }
    std::cout << "\n";

    // --- 3. loop: random joint-0 setpoint out, joint-0 state back ------------
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> q_dist{-kRandQ, kRandQ};
    std::uniform_real_distribution<float> dq_dist{-kRandDq, kRandDq};
    std::uniform_real_distribution<float> tau_dist{-kRandTau, kRandTau};

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + kRunTime;

    for (int sec = 0; g_run && std::chrono::steady_clock::now() < deadline; ++sec) {
        std::this_thread::sleep_for(1s);

        hand_sdk::frames::HandCommand joint0_command;
        joint0_command.q[1]   = q_dist(rng);
        joint0_command.dq[1]  = dq_dist(rng);
        joint0_command.tau[1] = tau_dist(rng);

        hand_sdk::frames::DualHandCommand command;
        command.has_left  = connected.value.has_left;
        command.left      = joint0_command;
        command.has_right = connected.value.has_right;
        command.right     = joint0_command;

        const auto sent = hand.sendCommand(command);

        std::cout << "[" << std::setw(3) << sec << "] write joint0 q="
                  << std::fixed << std::setprecision(3) << joint0_command.q[1]
                  << " dq=" << joint0_command.dq[1]
                  << " tau=" << joint0_command.tau[1];
        if (!sent.success) {
            std::cout << " (send failed: " << sent.msg << ")";
        }

        const auto state = hand.readState();
        if (state.success) {
            if (state.value.has_left)  { printState("L", state.value.left); }
            if (state.value.has_right) { printState("R", state.value.right); }
        } else {
            std::cout << "  read failed: " << state.msg;
        }
        std::cout << std::endl;
    }

    // --- 4. disconnect ------------------------------------------------------
    const auto stats = hand.cycleStats();
    hand.disconnect();

    std::cout << "\ncycles " << stats.cycles
              << " | exchange failures " << stats.exchange_failures
              << " | phase " << stats.phase_error_ns / 1000 << " us\n"
              << "left "  << hand.leftStatus().state_text
              << " | right " << hand.rightStatus().state_text << "\n";

    return 0;
}
