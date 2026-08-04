#pragma once

#include "config.hpp"
#include "status.hpp"
#include "messages.hpp"
#include "service.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
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
//
// Exception: the service*() calls are blocking request/response round-trips
// (~10 ms each — one 10-cycle bus window); do not call them from a real-time
// context.
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

    // Newest state published by the cyclic thread (the state holder): joint
    // states plus the per-motor error bits (HandState::error_bits). Fails
    // only if the bus has not published anything yet.
    Result<frames::DualHandState> readState();

    // Hand a command to the cyclic thread (the command holder); it goes out on the next
    // DC cycle. Returns as soon as the value is stored.
    Result<void> sendCommand(const frames::DualHandCommand& command);

    // Writes published into each holder so far. Compare across two calls to see
    // whether the bus has produced new state, or consumed your commands.
    uint64_t stateSequence() const;
    uint64_t commandSequence() const;

    // --- Service channel ----------------------------------------------------
    // Blocking request/response calls over the PDO service words. Each call
    // owns the channel for one 10-cycle window (~10 ms at 1 kHz). No queueing:
    // a call made while another is in flight fails immediately with
    // "another service call is in progress". See include/service.hpp for types.

    // cmd 4: start/stop broadcast responses. Write-only on the device, so
    // success means the request was held for the full window.
    Result<void> serviceComm(service::Hand hand, service::ComCmd cmd);

    // cmd 11: set closed-loop mode and control enable; returns the per-joint
    // mode/enable the device reports back.
    Result<service::ModeStatusArray> serviceMode(service::Hand hand,
                                                 service::LoopMode mode,
                                                 bool enable);

    // cmd 12 (read-only): per-joint motor-control state and fault code.
    Result<service::McStatusArray> serviceMcState(service::Hand hand);

    // cmd 14: set force-mode position loop gains (applied to all joints);
    // returns the per-joint gains the device reports back.
    Result<service::JointGainsArray> serviceGains(service::Hand hand,
                                                  float kp, float kd);

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

    // Serializes service calls (one window in flight) and guards service_seq_.
    std::mutex service_mutex_;
    uint64_t service_seq_{0};
};

} // namespace hand_sdk
