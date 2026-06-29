#include "../include/api.hpp"

#include <exception>

namespace hand_sdk
{

Api::Api(Config config) : connection_(std::move(config)) {}

Result<HandsStatus> Api::connect()
{
    try {
        auto err = connection_.connect();
        if (!err.empty()) {
            return {false, err};
        }

        return {true, "Connected", {connection_.hasLeft(), connection_.hasRight()}};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::update()
{
    try {
        if (!connection_.update()) {
            return {false, "PDO exchange failed"};
        }

        return {true, "OK"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::disconnect()
{
    try {
        connection_.disconnect();
        return {true, "Disconnected"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<frames::DualHandState> Api::readState()
{
    try {
        frames::DualHandState state{};
        if (!connection_.readState(state)) {
            return {false, "Failed to read state"};
        }

        return {true, "OK", state};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::sendCommand(const frames::DualHandCommand& command)
{
    try {
        if (!connection_.sendCommand(command)) {
            return {false, "Failed to send command"};
        }

        return {true, "OK"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

HandsStatus Api::isConnected() const
{
    return {connection_.hasLeft(), connection_.hasRight()};
}

} // namespace hand_sdk