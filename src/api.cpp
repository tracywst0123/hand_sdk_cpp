#include "../include/api.hpp"

#include "cyclic_task.hpp"
#include "frames/pdo_codec.hpp"
#include "frames/service_codec.hpp"
#include "service_report.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hand_sdk
{
namespace
{
// Step two of the write funnel and its wording; see service_report.hpp.
using service::report::checkJoints;
using service::report::jointList;
using service::report::hexWord;
using service::report::number;

// --- installation direction --------------------------------------------------
// Config::joint_direction, applied at the Api boundary and nowhere else: the
// frames below stay in device sign, everything above stays in model sign. The
// flip is its own inverse, so commands going out and state coming back run
// through the same function.
void flipDirections(const std::array<int8_t, frames::kNumJoints>& dirs,
                    std::array<float, frames::kNumJoints>& values)
{
    for (std::size_t i = 0; i < frames::kNumJoints; ++i) {
        if (dirs[i] < 0) {
            values[i] = -values[i];
        }
    }
}

template <typename JointSet>
void flipDirections(const std::array<int8_t, frames::kNumJoints>& dirs, JointSet& joints)
{
    flipDirections(dirs, joints.q);
    flipDirections(dirs, joints.dq);
    flipDirections(dirs, joints.tau);
}

// Which of the joints a request addressed can be held to an answer: the ones
// with a motor behind them. The reserved 21st slot goes out with the rest --
// the frame has to carry something there -- but nothing echoes for it, so it is
// dropped before the reply is checked rather than failing every write.
service::JointMask verifiable(service::JointMask joints)
{
    joints &= service::JointMask::active();
    return joints;
}

// The joints a mask holds, in order, for the report helpers -- which speak in
// index lists rather than bitmasks.
std::vector<std::size_t> selected(const service::JointMask& joints)
{
    std::vector<std::size_t> out;
    for (std::size_t j = 0; j < service::JointMask::kSize; ++j) {
        if (joints.test(j)) {
            out.push_back(j);
        }
    }
    return out;
}

// User threads are not real-time, so they can afford to keep retrying a torn
// read. The cyclic thread uses a much smaller limit; see Holder::tryRead().
constexpr int kUserReadAttempts = 64;

// A service window is 10 cycles (~10 ms at 1 kHz); the timeout leaves room
// for a window already in flight plus bus hiccups.
constexpr auto kServicePoll    = std::chrono::milliseconds(1);
constexpr auto kServiceTimeout = std::chrono::milliseconds(100);

// The zero-calibration message, grouped by state rather than listed per joint:
// twenty joints usually sit in one or two of the four codes, and "succeeded:
// 0-18 | calibrating: 19" says that in a line. Only the active joints are
// counted -- the reserved slot has no motor to calibrate.
std::string zeroCalSummary(const service::ZeroCalStateArray& states)
{
    using service::ZeroCalState;
    static constexpr ZeroCalState kOrder[] = {
        ZeroCalState::kSucceeded,
        ZeroCalState::kInProgress,
        ZeroCalState::kNotCalibrated,
        ZeroCalState::kFailed,
    };

    std::string msg = "zero-cal";
    std::vector<std::size_t> unknown;
    for (std::size_t j = 0; j < frames::kNumActiveJoints; ++j) {
        const bool listed = std::any_of(std::begin(kOrder), std::end(kOrder),
                                        [&](ZeroCalState s) { return states[j] == s; });
        if (!listed) {
            unknown.push_back(j);
        }
    }

    for (const auto state : kOrder) {
        std::vector<std::size_t> joints;
        for (std::size_t j = 0; j < frames::kNumActiveJoints; ++j) {
            if (states[j] == state) {
                joints.push_back(j);
            }
        }
        if (!joints.empty()) {
            msg += std::string(" | ") + service::stateName(state) + ": " +
                   jointList(joints);
        }
    }

    // A code the vendor table does not list is shown raw rather than folded
    // into one of the four -- it is the device saying something we do not know
    // how to read, which is worth seeing.
    if (!unknown.empty()) {
        msg += " | unknown:";
        for (std::size_t i = 0; i < unknown.size(); ++i) {
            msg += (i == 0 ? " j" : ", j") + std::to_string(unknown[i]) + "=" +
                   std::to_string(static_cast<int>(states[unknown[i]]));
        }
    }
    return msg;
}

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
// payload words, 12 per line (4 joints). The j-labels are *device slots*, not
// logical joints -- this is the wire, before frames/ un-flips each finger, so a
// request addressing logical j0 shows up under j00's fourth word (slot 3).
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
    const char* tag = ticket.left ? "L" : "R";
    dumpService(ticket.left ? "L request" : "R request", request);

    // Cycles lost before this call, so the failure paths below can say how many
    // were lost *during* it -- which is what separates "the wire dropped our
    // frames" from "the device said nothing".
    const uint64_t failures_before = task.stats().exchange_failures;
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
            // The window opened and closed on our id, but not one of ticks 5..9
            // produced a usable input frame -- every one of them had a short
            // working counter.
            std::clog << "[service] " << tag << " FAILED: window ran, nothing captured"
                      << " (id " << id << ", "
                      << (task.stats().exchange_failures - failures_before)
                      << " exchanges lost during the window)" << std::endl;
            return {false, "no reply from the targeted hand"};
        }
        std::this_thread::sleep_for(kServicePoll);
    }
    // No reply block carrying our id was ever published. The window is 10 cycles
    // at 1 kHz and the timeout is 100 ms, so this is not the window being slow:
    // it never opened, which means the cyclic thread never accepted the ticket.
    std::clog << "[service] " << tag << " FAILED: timed out, no window ran"
              << " (id " << id << ", "
              << (task.stats().exchange_failures - failures_before)
              << " exchanges lost while waiting)" << std::endl;
    return {false, "service reply timed out"};
}
} // namespace

Api::Api(Config config)
    : config_(config), task_(std::make_unique<CyclicTask>(std::move(config)))
{
}

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
        auto state = frames::pdo::decodeReadPDO(raw);
        if (state.has_left) {
            flipDirections(config_.joint_direction.left, state.left);
        }
        if (state.has_right) {
            flipDirections(config_.joint_direction.right, state.right);
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
        // Encode on the caller's thread — clamping and NaN guards included —
        // so the cyclic thread only ever copies ready-to-send frames. The sign
        // map is applied to a copy: the caller's command is theirs, and it is
        // in model sign.
        frames::DualHandCommand outgoing = command;
        if (outgoing.has_left) {
            flipDirections(config_.joint_direction.left, outgoing.left);
        }
        if (outgoing.has_right) {
            flipDirections(config_.joint_direction.right, outgoing.right);
        }
        task_->commands().write(frames::pdo::encodeWritePDO(outgoing));
        return {true, "OK"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Result<service::ComStatusArray> Api::serviceComm(service::Hand hand, service::ComCmd cmd)
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

    service::ComStatusArray echo{};
    if (!frames::pdo::decodeComm(raw.value, echo)) {
        return {false, "reply cmd mismatch (expected 4, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }

    // No mask on this one: encodeComm stacks the command into every joint slot,
    // so every motor was addressed and every motor is held to it.
    const auto want = static_cast<uint16_t>(cmd);
    const auto check = checkJoints(
        "comm", verifiable(service::JointMask::all()),
        [&](std::size_t j) { return service::took(echo[j], cmd); },
        [&](std::size_t j) { return hexWord(echo[j]) + " (wanted " + hexWord(want) + ")"; });
    return {check.ok, check.msg, echo};
}

Result<service::ZeroCalStateArray> Api::serviceZeroCalibrate(service::Hand hand,
                                                             service::ZeroCalDir dir,
                                                             service::JointMask joints)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (!joints.any()) {
        return {false, "no joints selected"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(
        *task_, hand, ++service_seq_,
        frames::pdo::encodeZeroCal(service::ZeroCalCmd::kStart, dir, joints));
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::ZeroCalStateArray out{};
    if (!frames::pdo::decodeZeroCal(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 10, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }

    // Deliberately no checkJoints() here. The other writes take effect inside
    // the window and echo their new state in it; this one kicks off a physical
    // routine that runs for seconds, so within the window there is nothing to
    // echo but "calibrating" at best. The reply landing on cmd 10 is the whole
    // verification -- the outcome is read later, with serviceZeroCalibrateState.
    return {true,
            "zero-cal requested on joints " + jointList(selected(verifiable(joints))) +
                " | " + zeroCalSummary(out),
            out};
}

Result<service::ZeroCalStateArray> Api::serviceZeroCalibrateState(service::Hand hand)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeZeroCalQuery());
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::ZeroCalStateArray out{};
    if (!frames::pdo::decodeZeroCal(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 10, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }
    // Nothing was asked to change, so the reply arriving is the success. What
    // the joints are actually reporting goes in the message.
    return {true, zeroCalSummary(out), out};
}

Result<service::ModeStatusArray> Api::serviceMode(service::Hand hand,
                                                  service::LoopMode mode,
                                                  bool enable,
                                                  service::JointMask joints)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (!joints.any()) {
        return {false, "no joints selected"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeMode(mode, enable, joints));
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::ModeStatusArray out{};
    if (!frames::pdo::decodeMode(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 11, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }

    // Mode and enable are enums: a joint either came back in the state it was
    // told to be in or it did not.
    const auto check = checkJoints(
        "mode", verifiable(joints),
        [&](std::size_t j) { return service::took(out[j], mode, enable); },
        [&](std::size_t j) {
            return "mode=" + std::to_string(static_cast<int>(out[j].mode)) +
                   " en=" + (out[j].enabled ? "1" : "0") + " (wanted mode=" +
                   std::to_string(static_cast<int>(mode)) + " en=" + (enable ? "1" : "0") + ")";
        });
    return {check.ok, check.msg, out};
}

Result<service::ModeStatusArray> Api::serviceModeQuery(service::Hand hand)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeModeQuery());
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
                                                   float kp, float kd,
                                                   service::JointMask joints)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (!joints.any()) {
        return {false, "no joints selected"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeGains(kp, kd, joints));
    if (!raw.success) {
        return {false, raw.msg};
    }
    service::JointGainsArray out{};
    if (!frames::pdo::decodeGains(raw.value, out)) {
        return {false, "reply cmd mismatch (expected 14, got " +
                           std::to_string(frames::pdo::cmdId(raw.value.cmd)) + ")"};
    }

    // The only numeric write: the readback is quantised, so it is checked
    // against a tolerance rather than for equality.
    const float tol = config_.service_tolerance;
    const auto check = checkJoints(
        "gains", verifiable(joints),
        [&](std::size_t j) { return service::took(out[j], kp, kd, tol); },
        [&](std::size_t j) {
            return "kp=" + number(out[j].kp) + " kd=" + number(out[j].kd) + " (wanted kp=" +
                   number(kp) + " kd=" + number(kd) + " +-" + number(tol) + ")";
        });
    return {check.ok, check.msg, out};
}

Result<service::JointGainsArray> Api::serviceGainsQuery(service::Hand hand)
{
    std::unique_lock<std::mutex> lock(service_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {false, "another service call is in progress"};
    }
    if (auto err = checkHand(*task_, hand); !err.success) {
        return {false, err.msg};
    }
    const auto raw = runService(*task_, hand, ++service_seq_,
                                frames::pdo::encodeGainsQuery());
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
