#pragma once

#include "api.hpp"
#include "config.hpp"

namespace hand_sdk
{

inline Api createEthercat(Config config)
{
    return Api(std::move(config));
}

} // namespace hand_sdk