#pragma once

#include "config.hpp"
#include "../src/connections/ethercat_connection.hpp"
#include "../src/frames/messages.hpp"

#include <string>

namespace hand_sdk
{

template <typename T>
struct Result
{
    bool success{false};
    std::string msg{};
    T value{};
};

template <>
struct Result<void>
{
    bool success{false};
    std::string msg{};
};

struct HandsStatus
{
    bool has_left{false};
    bool has_right{false};
};

class Api
{
public:
    explicit Api(Config config);

    Result<HandsStatus> connect();
    Result<void> disconnect();

    // Exchange PDO buffers. Call at a fixed rate independent of readState/sendCommand.
    Result<void> update();

    Result<frames::DualHandState> readState();
    Result<void> sendCommand(const frames::DualHandCommand& command);

    HandsStatus isConnected() const;

private:
    connections::EthercatConnection connection_;
};

} // namespace hand_sdk