#include "../include/hand_sdk.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_run{true};
void onSignal(int) { g_run = false; }

// Print the first 6 PDO "words" of a hand state: joints[0] and joints[1],
// each as (q=angle, dq=speed, tau=torque). These are the first 6 int16 values.
void printFirst6(const char* tag, const hand_sdk::frames::HandState& s)
{
    std::cout << " | " << tag << ":"
              << " [0] q=" << s.q[0] << " dq=" << s.dq[0] << " tau=" << s.tau[0]
              << " [1] q=" << s.q[1] << " dq=" << s.dq[1] << " tau=" << s.tau[1];
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

    auto hand = hand_sdk::createEthercat(config);

    auto connect_result = hand.connect();
    if (!connect_result.success) {
        std::cerr << "Connect failed: " << connect_result.msg << "\n";
        return 1;
    }

    std::cout << "Left hand:  " << (connect_result.value.has_left  ? "found" : "not found") << "\n";
    std::cout << "Right hand: " << (connect_result.value.has_right ? "found" : "not found") << "\n";


    hand_sdk::frames::DualHandCommand command{};
    command.has_left  = true;
    command.left.q[0]   = 20;  // 1st word
    command.left.dq[0]  = 20;  // 2nd word
    command.left.tau[0] = 20;  // 3rd word
    command.has_right = true;
    command.right.q[0]   = 20;
    command.right.dq[0]  = 20;
    command.right.tau[0] = 20;

    std::cout << "Running echo test (Ctrl-C to stop)...\n";
    std::cout << "Write increments to left, 20 to right (q[0], dq[0], tau[0]).\n";

    constexpr int kPrintEveryN = 100;  // ~10 Hz at a 1 ms cycle
    int cycle = 0;

    while (g_run) {

        hand.sendCommand(command);

        command.left.q[0] += 1;   // increment the command for the next cycle`
        command.left.dq[0] += 1;
        command.left.tau[0] += 1;
        auto update_result = hand.update();
        auto state_result  = hand.readState();

        if (cycle % kPrintEveryN == 0) {
            std::cout << "[" << cycle << "] update=" << update_result.success;
            if (state_result.success) {
                if (state_result.value.has_left) {
                    printFirst6("L", state_result.value.left);
                }
                if (state_result.value.has_right) {
                    printFirst6("R", state_result.value.right);
                }
            } else {
                std::cout << " (readState failed: " << state_result.msg << ")";
            }
            std::cout << std::endl;
        }

        ++cycle;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "\nStopping...\n";
    hand.disconnect();
    return 0;
}
