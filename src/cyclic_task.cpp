#include "cyclic_task.hpp"

#include "frames/pdo_codec.hpp"
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

    // Seed the command holder before the thread exists, so the very first cycle sends a
    // defined command rather than whatever the user has not written yet. Both
    // has_ flags are false, so nothing is staged until the user writes.
    commands_.write(frames::pdo::DualWritePDO{});

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
    // rather than skip a cycle, because the bus must be fed either way. Raw
    // wire frames — this thread never runs the codec, only copies.
    frames::pdo::DualWritePDO command{};
    frames::pdo::DualReadPDO state{};

    // The frame actually staged each cycle: joint words from the newest user
    // command, service words owned by the service window below. Hands the
    // user has not addressed keep their previous joint words (zeros at
    // start), matching what the out buffers would hold anyway.
    frames::pdo::DualWritePDO wire{};
    wire.has_left  = true;  // presence is gated per slave in stageOutputs()
    wire.has_right = true;

    // Service window state. Baseline on any ticket left over from a previous
    // run, so it is not replayed; -1 = no window open.
    const frames::pdo::ServiceData default_service = frames::pdo::defaultServiceData();
    ServiceTicket ticket{};
    ServiceReply reply{};
    uint64_t done_id = 0;
    if (service_requests_.tryRead(ticket, 4) != 0) {
        done_id = ticket.id;
    }
    int service_cycle = -1;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        timer.waitNextCycle();

        // Command holder -> joint words. A zero here means the user thread was
        // mid-write; the previous command stands for one more cycle.
        if (commands_.tryRead(command, 4) == 0) {
            command_underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        if (command.has_left) {
            wire.left = command.left;
        }
        if (command.has_right) {
            wire.right = command.right;
        }

        // Service window: accept a new ticket only when idle, then hold its
        // request in the service words for 10 consecutive cycles so the
        // slaves are guaranteed to see it (notes.md, "Service channel").
        if (service_cycle < 0 && service_requests_.tryRead(ticket, 2) != 0 &&
            ticket.id != 0 && ticket.id != done_id) {
            service_cycle = 0;
        }
        const bool window_open = service_cycle >= 0;
        wire.left.service_data =
            (window_open && ticket.left) ? ticket.request : default_service;
        wire.right.service_data =
            (window_open && ticket.right) ? ticket.request : default_service;

        connection_.stageOutputs(wire);

        if (!connection_.update()) {
            exchange_failures_.fetch_add(1, std::memory_order_relaxed);
        }

        // Wire -> state holder.
        const bool inputs_ok = connection_.copyInputs(state);
        if (inputs_ok) {
            states_.write(state);
        }

        // Record the reply over the 7th..9th frames of the window (the slaves
        // need several frames to publish it; overwriting keeps the newest good
        // copy, so a missed 9th frame still leaves the 8th). The final copy is
        // published once, when the window closes.
        if (window_open) {
            if (inputs_ok && service_cycle >= 6 && service_cycle <= 8) {
                if (ticket.left && state.has_left) {
                    reply.has_left = true;
                    reply.left     = state.left.service_data;
                }
                if (ticket.right && state.has_right) {
                    reply.has_right = true;
                    reply.right     = state.right.service_data;
                }
            }
            if (++service_cycle >= 10) {
                // id is stamped even if nothing was captured: the has_ flags
                // then tell Api "no reply" instead of leaving it to time out.
                reply.id = ticket.id;
                service_replies_.write(reply);
                done_id = ticket.id;
                reply         = {};
                service_cycle = -1;  // window closed; back to the default block
            }
        }

        timer.syncTo(connection_.dcTime());
        phase_error_ns_.store(timer.phaseError(), std::memory_order_relaxed);
        cycles_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace hand_sdk
