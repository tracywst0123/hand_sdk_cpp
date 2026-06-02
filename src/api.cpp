#include "api.hpp"

#include <exception>

namespace hand_sdk
{

Api::Api(std::unique_ptr<connections::BaseConnection> connection) : connection_(std::move(connection)) {}

Result<void> Api::connect()
{
    try {
        if (!connection_) {
            return {false, "No connection"};
        }

        if (!connection_->connect()) {
            return {false, "Failed to connect"};
        }

        return {true, "Connected"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::disconnect()
{
    try {
        if (!connection_) {
            return {false, "No connection"};
        }

        connection_->disconnect();
        return {true, "Disconnected"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<frames::ReadState> Api::readState()
{
    try {
        if (!connection_ || !connection_->isConnected()) {
            return {false, "Not connected", {}};
        }

        frames::ReadState state{};
        if (!connection_->readState(state)) {
            return {false, "Failed to read state", {}};
        }

        return {true, "Read state OK", state};
    } catch (const std::exception& e) {
        return {false, e.what(), {}};
    }
}

Result<void> Api::sendCommand(const frames::QCommand& command)
{
    try {
        if (!connection_ || !connection_->isConnected()) {
            return {false, "Not connected"};
        }

        if (!connection_->sendCommand(command)) {
            return {false, "Failed to send command"};
        }

        return {true, "Command sent"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

bool Api::isConnected() const
{
    return connection_ && connection_->isConnected();
}

std::string Api::connectionName() const
{
    return connection_ ? connection_->name() : "none";
}

} // namespace hand_sdk