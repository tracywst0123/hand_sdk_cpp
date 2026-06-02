#pragma once

#include "../src/api.hpp"
#include "../src/connections/ethercat_connection.hpp"

#include <memory>
#include <string>

namespace hand_sdk
{

class HandSdk
{
public:
    static Api createEthercat(const std::string& interface_name)
    {
        return Api(std::make_unique<connections::EthercatConnection>(interface_name));
    }

};

} // namespace hand_sdk