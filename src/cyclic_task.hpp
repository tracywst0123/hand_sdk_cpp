#pragma once

#include "../include/config.hpp"
#include "holder.hpp"
#include "../include/status.hpp"
#include "connections/ethercat_connection.hpp"
#include "../include/messages.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace hand_sdk
{

// The bottom layer.
//
// Owns the EtherCAT connection and a real-time thread that, once per DC cycle:
//   1. takes the newest command out of `commands()`  (holder2)
//   2. exchanges it on the wire
//   3. publishes the state that came back into `states()`  (holder1)
//
// Nothing above this class touches the connection or the wire. The user layer
// only reads holder1 and writes holder2, which is what decouples control-code
// timing from bus timing: user code can run at 50 Hz, or stall for a whole
// second, and the bus still gets fed every millisecond on the DC grid.
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

    // holder1: bus -> user. Written only by the cyclic thread.
    Holder<frames::DualHandState>& states() { return states_; }
    const Holder<frames::DualHandState>& states() const { return states_; }

    // holder2: user -> bus. Written only by user threads.
    Holder<frames::DualHandCommand>& commands() { return commands_; }
    const Holder<frames::DualHandCommand>& commands() const { return commands_; }

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

    Holder<frames::DualHandState> states_;
    Holder<frames::DualHandCommand> commands_;

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
