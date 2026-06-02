#pragma once

#include "connections/base_connection.hpp"
#include "frames/messages.hpp"

#include <memory>
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

class Api
{
public:
    explicit Api(std::unique_ptr<connections::BaseConnection> connection);

    Result<void> connect();
    Result<void> disconnect();

    Result<frames::ReadState> readState();
    Result<void> sendCommand(const frames::QCommand& command);

    bool isConnected() const;
    std::string connectionName() const;

private:
    std::unique_ptr<connections::BaseConnection> connection_;
};

} // namespace hand_sdk