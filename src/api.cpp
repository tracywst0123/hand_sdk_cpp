#include "../include/api.hpp"

#include "cyclic_task.hpp"

#include <exception>
#include <utility>

namespace hand_sdk
{
namespace
{
// User threads are not real-time, so they can afford to keep retrying a torn
// read. The cyclic thread uses a much smaller limit; see Holder::tryRead().
constexpr int kUserReadAttempts = 64;
} // namespace

Api::Api(Config config) : task_(std::make_unique<CyclicTask>(std::move(config))) {}

// Out of line: CyclicTask is incomplete at the point of the class definition.
Api::~Api() = default;

Result<HandsStatus> Api::connect()
{
    try {
        if (auto err = task_->start(); !err.empty()) {
            return {false, err};
        }
        return {true, "Connected", {task_->hasLeft(), task_->hasRight()}};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::disconnect()
{
    try {
        task_->stop();
        return {true, "Disconnected"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<frames::DualHandState> Api::readState()
{
    try {
        // Checked before the read, not after: once the cyclic thread is stopped
        // the holder still contains its last value, and handing that back as a
        // success would be indistinguishable from live data.
        if (!task_->isRunning()) {
            return {false, "not connected"};
        }
        frames::DualHandState state{};
        if (task_->states().tryRead(state, kUserReadAttempts) == 0) {
            return {false, "no state published yet"};
        }
        return {true, "OK", state};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::sendCommand(const frames::DualHandCommand& command)
{
    try {
        if (!task_->isRunning()) {
            return {false, "not connected"};
        }
        task_->commands().write(command);
        return {true, "OK"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

uint64_t Api::stateSequence() const
{
    return task_->states().sequence();
}

uint64_t Api::commandSequence() const
{
    return task_->commands().sequence();
}

HandsStatus Api::isConnected() const
{
    return {task_->hasLeft(), task_->hasRight()};
}

CycleStats Api::cycleStats() const
{
    return task_->stats();
}

bool Api::dcActive() const
{
    return task_->dcActive();
}

const SyncDiagnostics& Api::leftDiagnostics() const
{
    return task_->leftDiagnostics();
}

const SyncDiagnostics& Api::rightDiagnostics() const
{
    return task_->rightDiagnostics();
}

const SlaveStatus& Api::leftStatus() const
{
    return task_->leftStatus();
}

const SlaveStatus& Api::rightStatus() const
{
    return task_->rightStatus();
}

} // namespace hand_sdk
