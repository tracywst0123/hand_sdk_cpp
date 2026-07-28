#pragma once

#include "config.hpp"
#include "status.hpp"
#include "messages.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hand_sdk
{

// The bottom layer, held by pointer so that neither SOEM nor the EtherCAT
// connection appears in this header. User code — a ROS node, say — can include
// the SDK without needing the master stack's include paths, and genuinely has
// no way to reach past the API to the wire.
class CyclicTask;

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

// The user layer.
//
// Every call here is a handoff to or from the cyclic thread's holders — none of
// them touch the EtherCAT bus. That means they never block on the wire, never
// have to be called at any particular rate, and are safe from a ROS callback or
// any ordinary thread. The bus keeps running on its own DC-locked millisecond
// whether you call these once a second or ten thousand times a second.
//
// The trade is that reads and writes are decoupled from bus cycles: readState()
// gives you the newest state the bus has published, which may be the same one
// you saw last call, and sendCommand() is delivered on the next cycle rather
// than immediately. Use the sequence counters to tell those apart when it
// matters.
class Api
{
public:
    explicit Api(Config config);
    ~Api();

    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    // Bring up the bus and start the cyclic thread.
    Result<HandsStatus> connect();

    // Stop cycling and drop the bus. Also called by the destructor.
    Result<void> disconnect();

    // --- Data path ----------------------------------------------------------

    // Newest state published by the cyclic thread (holder1). Fails only if the
    // bus has not published anything yet.
    Result<frames::DualHandState> readState();

    // Hand a command to the cyclic thread (holder2); it goes out on the next
    // DC cycle. Returns as soon as the value is stored.
    Result<void> sendCommand(const frames::DualHandCommand& command);

    // Writes published into each holder so far. Compare across two calls to see
    // whether the bus has produced new state, or consumed your commands.
    uint64_t stateSequence() const;
    uint64_t commandSequence() const;

    // --- Status -------------------------------------------------------------

    HandsStatus isConnected() const;
    CycleStats cycleStats() const;
    bool dcActive() const;

    // The devices' own verdict on whether we met the SYNC deadlines, captured
    // during disconnect(). Empty (valid == false) before then.
    const SyncDiagnostics& leftDiagnostics() const;
    const SyncDiagnostics& rightDiagnostics() const;

    // Where each hand sits in the EtherCAT state machine, as of the last
    // connect() or disconnect(). Not refreshed while cycling — see SlaveStatus.
    const SlaveStatus& leftStatus() const;
    const SlaveStatus& rightStatus() const;

private:
    std::unique_ptr<CyclicTask> task_;
};

} // namespace hand_sdk
