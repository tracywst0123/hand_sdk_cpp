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
    //
    // The two read services (serviceModeQuery, serviceMcState) succeed once a
    // reply block for the hand comes back with the right cmd id -- there is
    // nothing else to check, since nothing was asked to change.
    //
    // The three write services (serviceComm, serviceMode, serviceGains) put
    // that same transport check first and then verify the reply per joint:
    // every joint the request addressed must echo what it was told, exactly for
    // the enum writes and within Config::service_tolerance for gains. One
    // addressed joint disagreeing fails the call, and msg then names the joints
    // on each side with what came back. The decoded reply is still returned in
    // value, so a caller that wants to render its own per-joint view does not
    // have to read the message.
    //
    // The reserved 21st slot is written like the others but never verified --
    // it has no motor behind it to echo (JointMask::active()).

    // cmd 4: start/stop broadcast responses. Addressed to every joint slot, so
    // all of them are held to the echoed command.
    Result<void> serviceComm(service::Hand hand, service::ComCmd cmd);

    // cmd 11: set closed-loop mode and control enable on the selected joints
    // (default: all of them). Joints outside the mask get the device's "no
    // action" word, keep whatever they were set to, and are not verified.
    // Returns the per-joint mode/enable the device reports back.
    Result<service::ModeStatusArray> serviceMode(
        service::Hand hand,
        service::LoopMode mode,
        bool enable,
        service::JointMask joints = service::JointMask::all());

    // cmd 11 as a query: every payload slot carries the ignore word, so nothing
    // is written and the reply reports the mode/enable each joint is already in.
    Result<service::ModeStatusArray> serviceModeQuery(service::Hand hand);

    // cmd 12 (read-only): per-joint motor-control state and fault code.
    Result<service::McStatusArray> serviceMcState(service::Hand hand);

    // cmd 14: set force-mode position loop gains on the selected joints
    // (default: all), same masking rule as serviceMode. The only numeric write:
    // its readback is quantised (s16en11), so an addressed joint passes when it
    // lands within Config::service_tolerance of the request. Returns the
    // per-joint gains the device reports back.
    Result<service::JointGainsArray> serviceGains(
        service::Hand hand,
        float kp, float kd,
        service::JointMask joints = service::JointMask::all());

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
    // Kept because the write services verify their replies against it
    // (Config::service_tolerance). Declared before task_, which the constructor
    // moves the original into.
    Config config_;

    std::unique_ptr<CyclicTask> task_;

    // Serializes service calls (one window in flight) and guards service_seq_.
    std::mutex service_mutex_;
    uint64_t service_seq_{0};
};

} // namespace hand_sdk
