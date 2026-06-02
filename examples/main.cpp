#include "../src/hand_sdk.hpp"

#include <iostream>

int main()
{
    auto hand = hand_sdk::HandSdk::createEthercat("eth0");

    auto connect_result = hand.connect();
    if (!connect_result.success) {
        std::cerr << connect_result.msg << "\n";
        return 1;
    }

    hand_sdk::frames::QCommand command{};
    command.mode = hand_sdk::frames::ControlMode::Position;
    command.q[0] = 0.5F;

    auto send_result = hand.sendCommand(command);
    std::cout << send_result.msg << "\n";

    auto state_result = hand.readState();
    if (state_result.success) {
        std::cout << "q[0] = " << state_result.value.q[0] << "\n";
    }

    hand.disconnect();
    return 0;
}