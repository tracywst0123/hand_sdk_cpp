#include "cyclic_task.hpp"

#include "realtime.hpp"

#include <iostream>
#include <utility>

namespace hand_sdk
{

CyclicTask::CyclicTask(Config config)
    : config_(std::move(config)), connection_(config_)
{
}

CyclicTask::~CyclicTask()
{
    stop();
}

std::string CyclicTask::start()
{
    if (isRunning()) {
        return {};
    }

    if (auto err = connection_.connect(); !err.empty()) {
        return err;
    }

    // Seed holder2 before the thread exists, so the very first cycle sends a
    // defined command rather than whatever the user has not written yet.
    commands_.write(frames::DualHandCommand{});

    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&CyclicTask::run, this);

    return {};
}

void CyclicTask::stop()
{
    if (thread_.joinable()) {
        stop_requested_.store(true, std::memory_order_release);
        thread_.join();
    }
    running_.store(false, std::memory_order_release);

    // Thread is joined, so the bus is ours alone: safe to spend milliseconds on
    // mailbox traffic now. Capture before disconnect() closes the port.
    if (connection_.dcActive()) {
        if (connection_.hasLeft()) {
            left_diag_ = connection_.syncDiagnostics(connection_.leftSlaveId());
        }
        if (connection_.hasRight()) {
            right_diag_ = connection_.syncDiagnostics(connection_.rightSlaveId());
        }
    }

    connection_.disconnect();
}

CycleStats CyclicTask::stats() const
{
    CycleStats s;
    s.cycles            = cycles_.load(std::memory_order_relaxed);
    s.exchange_failures = exchange_failures_.load(std::memory_order_relaxed);
    s.command_underruns = command_underruns_.load(std::memory_order_relaxed);
    s.phase_error_ns    = phase_error_ns_.load(std::memory_order_relaxed);
    s.running           = running_.load(std::memory_order_acquire);
    return s;
}

void CyclicTask::run()
{
    // Priority goes on this thread, not on whoever called start(). The cycle is
    // what has a deadline; user code should stay preemptible.
    if (const auto problem = rt::configureRealtime(config_.rt_priority); !problem.empty()) {
        std::clog << "[cyclic] " << problem << std::endl;
    }

    rt::CyclicTimer timer(config_.dc.sync0_cycle_ns, config_.dc.master_offset_ns);

    // Held across iterations: if a holder read loses its race we resend these
    // rather than skip a cycle, because the bus must be fed either way.
    frames::DualHandCommand command{};
    frames::DualHandState state{};

    while (!stop_requested_.load(std::memory_order_acquire)) {
        timer.waitNextCycle();

        // holder2 -> wire. A zero here means the user thread was mid-write; the
        // previous command stands for one more cycle.
        if (commands_.tryRead(command, 4) == 0) {
            command_underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        connection_.stageCommand(command);

        if (!connection_.update()) {
            exchange_failures_.fetch_add(1, std::memory_order_relaxed);
        }

        // wire -> holder1.
        if (connection_.readState(state)) {
            states_.write(state);
        }

        timer.syncTo(connection_.dcTime());
        phase_error_ns_.store(timer.phaseError(), std::memory_order_relaxed);
        cycles_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace hand_sdk
