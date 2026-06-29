#include "../include/hand_sdk.hpp"

#include <iostream>
#include <thread>

int main()
{
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

    // Send a position command to the left hand
    hand_sdk::frames::DualHandCommand command{};
    command.has_right   = true;
    command.right.dq[0]  = 20;  // joint 1 position in raw int16 units

    command.has_left   = true;
    command.left.dq[0]  = 20;  // joint 1 position in raw int16 units
    hand.sendCommand(command);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto update_result = hand.update();
    std::cout << "update: " << update_result.success << "\n";
    std::cout << "update: " << update_result.msg << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    update_result = hand.update();
    std::cout << "update: " << update_result.success << "\n";
    std::cout << "update: " << update_result.msg << "\n";

    // Read state from both hands (triggers a PDO exchange)
    auto state_result = hand.readState();
    if (state_result.success) {
        if (state_result.value.has_right) {
            for (int i = 0; i < 21; ++i) {
                std::cout << "Right q[" << i << "]=" << state_result.value.right.q[i]
                          << " dq[" << i << "]=" << state_result.value.right.dq[i]
                          << " tau[" << i << "]=" << state_result.value.right.tau[i] << "\n";
            }
        }
    }

    hand.disconnect();
    return 0;
}