#include "../include/api.hpp"

#include "cyclic_task.hpp"
#include "frames/pdo_codec.hpp"
#include "frames/service_codec.hpp"

#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

namespace hand_sdk
{
namespace
{
// User threads are not real-time, so they can afford to keep retrying a torn
// read. The cyclic thread uses a much smaller limit; see Holder::tryRead().
constexpr int kUserReadAttempts = 64;

// A service window is 10 cycles (~10 ms at 1 kHz); the timeout leaves room
// for a window already in flight plus bus hiccups.
constexpr auto kServicePoll    = std::chrono::milliseconds(1);
constexpr auto kServiceTimeout = std::chrono::milliseconds(100);

Result<void> checkHand(CyclicTask& task, service::Hand hand)
{
    if (!task.isRunning()) {
        return {false, "not connected"};
    }
    if (hand == service::Hand::kLeft && !task.hasLeft()) {
        return {false, "left hand not connected"};
    }
    if (hand == service::Hand::kRight && !task.hasRight()) {
        return {false, "right hand not connected"};
    }
    return {true, {}};
}

// Raw service block as it goes on / comes off the wire: cmd word then the 63
// payload words, 12 per line (4 joints).
void dumpService(const char* tag, const frames::pdo::ServiceData& sd)
{
    std::ostringstream oss;
    oss << "[service] " << tag << " cmd=0x" << std::hex << std::setw(4) << std::setfill('0')
        << static_cast<uint16_t>(sd.cmd) << std::dec
        << " (" << (frames::pdo::cmdIsWrite(sd.cmd) ? 'w' : 'r')
        << " id " << static_cast<int>(frames::pdo::cmdId(sd.cmd)) << ")";

    for (std::size_t i = 0; i < frames::pdo::kJointWords; ++i) {
        if (i % 12 == 0) {
            oss << "\n  j" << std::setw(2) << std::setfill('0') << std::dec << (i / 3) << ": ";
        }
        oss << ' ' << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<uint16_t>(sd.data[i]);
    }

    std::clog << oss.str() << std::dec << std::endl;
}

// One service round-trip: post the ticket, then poll the reply holder until
// the cyclic thread captures a reply with our id. Caller holds service_mutex_
// and has checked hand presence.
Result<frames::pdo::ServiceData> runService(CyclicTask& task,
                                            service::Hand hand,
                                            uint64_t id,
                                            const frames::pdo::ServiceData& request)
{
    ServiceTicket ticket{};
    ticket.id      = id;
    ticket.left    = hand == service::Hand::kLeft;
    ticket.right   = hand == service::Hand::kRight;
    ticket.request = request;
    dumpService(ticket.left ? "L request" : "R request", request);
    task.serviceRequests().write(ticket);

    const auto deadline = std::chrono::steady_clock::now() + kServiceTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!task.isRunning()) {
            return {false, "connection lost during service call"};
        }
        ServiceReply reply{};
        if (task.serviceReplies().tryRead(reply, kUserReadAttempts) != 0 &&
            reply.id == id) {
            if (ticket.left && reply.has_left) {
                dumpService("L reply  ", reply.left);
                return {true, "OK", reply.left};
            }
            if (ticket.right && reply.has_right) {
                dumpService("R reply  ", reply.right);
                return {true, "OK", reply.right};
            }
            return {false, "no reply from the targeted hand"};
        }
        std::this_thread::sleep_for(kServicePoll);
    }
    return {false, "service reply timed out"};
}
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
        // The holder hands over raw wire frames; decoding to floats happens
        // here, on the caller's thread, keeping the cyclic loop copy-only.
        frames::pdo::DualReadPDO raw{};
        if (task_->states().tryRead(raw, kUserReadAttempts) == 0) {
            return {false, "no state published yet"};
        }
        return {true, "OK", frames::pdo::decodeReadPDO(raw)};
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
        // Encode on the caller's thread — clamping and NaN guards included —
        // so the cyclic thread only ever copies ready-to-send frames.
        task_->commands().write(frames::pdo::encodeWritePDO(command));
        return {true, "OK"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<void> Api::serviceComm(service::Hand hand, service::ComCmd cmd)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeComm(cmd));
    if (!raw.success) {
        return {false, raw.msg};
    }
    // Write-only service: the reply carries no meaningful payload.
    return {true, raw.msg};
}

Result<service::ModeStatusArray> Api::serviceMode(service::Hand hand,
                                                  service::LoopMode mode,
                                                  bool enable)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeMode(mode, enable));
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::ModeStatusArray out{};
    if (!frames::pdo::decodeMode(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 11, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }
    return {true, "OK", out};
}

Result<service::McStatusArray> Api::serviceMcState(service::Hand hand)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeMcQuery());
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::McStatusArray out{};
    if (!frames::pdo::decodeMc(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 12, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }
    return {true, "OK", out};
}

Result<service::JointGainsArray> Api::serviceGains(service::Hand hand,
                                                   float kp, float kd)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeGains(kp, kd));
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::JointGainsArray out{};
    if (!frames::pdo::decodeGains(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 14, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }
    return {true, "OK", out};
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
