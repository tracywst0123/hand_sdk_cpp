#pragma once

#include "../frames/messages.hpp"

#include <string>

namespace hand_sdk::connections
{

class BaseConnection
{
public:
    virtual ~BaseConnection() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual bool sendCommand(const frames::QCommand& command) = 0;
    virtual bool readState(frames::ReadState& state) = 0;

    virtual std::string name() const = 0;
};

} // namespace hand_sdk::connections