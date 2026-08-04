#pragma once

#include "../include/config.hpp"
#include "../include/status.hpp"
#include "connections/ethercat_connection.hpp"
#include "holder.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace hand_sdk
{

// A service request handed to the cyclic thread. Written by user threads
// (serialized in Api); `id` makes each call distinguishable from the last.
struct ServiceTicket
{
    uint64_t id{0};  // 0 = empty
    bool left{false};
    bool right{false};
    frames::pdo::ServiceData request{};
};

// The reply captured for a ticket, tagged with the same id.
struct ServiceReply
{
    uint64_t id{0};
    bool has_left{false};
    bool has_right{false};
    frames::pdo::ServiceData left{};
    frames::pdo::ServiceData right{};
};

// The bottom layer.
//
// Owns the EtherCAT connection and a real-time thread that, once per DC cycle:
//   1. takes the newest command frames out of `commands()`, the command holder
//   2. exchanges them on the wire
//   3. publishes the state frames that came back into `states()`, the state holder
//
// Nothing above this class touches the connection or the wire. The user layer
// only reads the state holder and writes the command holder, which decouples
// control-code timing from bus timing: user code can run at 50 Hz, or stall
// for a whole second, and the bus still gets fed every millisecond.
//
// The holders carry *wire-format* frames (pdo::DualWritePDO / pdo::DualReadPDO),
// already encoded / not yet decoded. Api does the float<->int16 codec work on
// the calling user thread, so the cyclic loop below is nothing but memcpys —
// the whole RT cycle is copies of fixed-size PODs, no arithmetic.
class CyclicTask
{
public:
    explicit CyclicTask(Config config);
    ~CyclicTask();

    CyclicTask(const CyclicTask&) = delete;
    CyclicTask& operator=(const CyclicTask&) = delete;

    // Bring up the bus, then start cycling. Empty string on success.
    std::string start();

    // Stop cycling, capture the sync diagnostics, then drop the bus. Idempotent.
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // State holder, bus -> user. Written only by the cyclic thread.
    Holder<frames::pdo::DualReadPDO>& states() { return states_; }
    const Holder<frames::pdo::DualReadPDO>& states() const { return states_; }

    // Command holder, user -> bus. Written only by user threads.
    Holder<frames::pdo::DualWritePDO>& commands() { return commands_; }
    const Holder<frames::pdo::DualWritePDO>& commands() const { return commands_; }

    // Service channel, one request/response in flight at a time (Api rejects
    // a call while one runs). The cyclic thread holds a ticket's request in
    // the outgoing service words for a 10-cycle window, records the reply
    // over frames 7-9, publishes the final copy when the window closes, then
    // reverts to the default block (cmd 0).
    Holder<ServiceTicket>& serviceRequests() { return service_requests_; }
    Holder<ServiceReply>& serviceReplies() { return service_replies_; }

    CycleStats stats() const;

    bool hasLeft() const { return connection_.hasLeft(); }
    bool hasRight() const { return connection_.hasRight(); }
    bool dcActive() const { return connection_.dcActive(); }

    // Captured during stop(), while the cyclic thread is already joined — SDO
    // traffic from another thread mid-run would add mailbox datagrams to the
    // cycle and cost us the deadline we are trying to measure.
    const SyncDiagnostics& leftDiagnostics() const { return left_diag_; }
    const SyncDiagnostics& rightDiagnostics() const { return right_diag_; }

    // AL state as of the last connect()/disconnect(); see SlaveStatus.
    const SlaveStatus& leftStatus() const { return connection_.leftStatus(); }
    const SlaveStatus& rightStatus() const { return connection_.rightStatus(); }

private:
    void run();

    Config config_;
    connections::EthercatConnection connection_;

    Holder<frames::pdo::DualReadPDO> states_;
    Holder<frames::pdo::DualWritePDO> commands_;

    Holder<ServiceTicket> service_requests_;
    Holder<ServiceReply> service_replies_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    std::atomic<uint64_t> cycles_{0};
    std::atomic<uint64_t> exchange_failures_{0};
    std::atomic<uint64_t> command_underruns_{0};
    std::atomic<int64_t> phase_error_ns_{0};

    SyncDiagnostics left_diag_{};
    SyncDiagnostics right_diag_{};
};

} // namespace hand_sdk
