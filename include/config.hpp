#pragma once

#include <string>

namespace hand_sdk
{

struct Config
{
    std::string interface_name{"enp132s0"};
    std::string left_hand_name{"hand_200_L"};
    std::string right_hand_name{"hand_200_R"};
};

} // namespace hand_sdk
