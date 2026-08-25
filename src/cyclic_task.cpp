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

        // The device's own account of the run. 0x1C32:0B / 0x1C33:0B count the
        // cycles where its application was still busy when the next SYNC0 came
        // -- i.e. the slave missed its deadline, not us. Nonzero here separates
        // "the hand choked on what we sent" from "we stopped feeding it", which
        // is the whole question when a service call costs us OP.
        const auto report = [](const char* label, const SyncDiagnostics& d) {
            if (!d.valid) {
                return;
            }
            std::clog << "[cyclic] " << label << " sync diagnostics:"
                      << " sync type out/in " << d.out_sync_type << "/" << d.in_sync_type
                      << " | cycle exceeded out/in " << d.out_cycle_exceeded << "/"
                      << d.in_cycle_exceeded
                      << " | shift too short out/in " << d.out_shift_too_short << "/"
                      << d.in_shift_too_short;
            if (d.out_cycle_exceeded != 0 || d.in_cycle_exceeded != 0) {
                std::clog << "  <-- the device missed its own SYNC0 deadline";
            }
            std::clog << std::endl;
        };
        report("left", left_diag_);
        report("right", right_diag_);
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

    // Bus health, reported on the edges. Without this a dropout is invisible
    // until some later service call comes back empty, which reads like a
    // service fault when it is really the wire or the state machine: a link
    // glitch longer than the slave's SM watchdog knocks it out of OP, and from
    // then on the output FPWR stops being counted, so every exchange is short
    // and every service window captures nothing.
    //
    // Edge-triggered because probeOperational() puts datagrams on the wire: it
    // runs on the transition into failure -- where the cycle is already blown
    // anyway -- and then at most once a second for as long as it stays broken.
    bool exchange_healthy = true;
    uint64_t cycle = 0;
    uint64_t last_probe_cycle = 0;
    const uint64_t probe_interval_cycles =
        config_.dc.sync0_cycle_ns > 0
            ? static_cast<uint64_t>(1000000000LL / config_.dc.sync0_cycle_ns)
            : 1000;

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

        const bool exchange_ok = connection_.update();
        if (!exchange_ok) {
            exchange_failures_.fetch_add(1, std::memory_order_relaxed);
            if (exchange_healthy || cycle - last_probe_cycle >= probe_interval_cycles) {
                std::string where;
                connection_.probeOperational(where);
                std::clog << "[cyclic] bus not answering at cycle " << cycle << ": " << where
                          << " (" << exchange_failures_.load(std::memory_order_relaxed)
                          << " exchanges lost so far)" << std::endl;
                last_probe_cycle = cycle;
            }
            exchange_healthy = false;
        } else if (!exchange_healthy) {
            std::string where;
            const bool op = connection_.probeOperational(where);
            std::clog << "[cyclic] bus answering again at cycle " << cycle << ": " << where
                      << (op ? "" : " -- exchanging but NOT in OP, service replies will be stale")
                      << std::endl;
            exchange_healthy = true;
        }

        // Wire -> state holder.
        const bool inputs_ok = connection_.copyInputs(state);
        if (inputs_ok) {
            states_.write(state);
        }

        // Record the reply over ticks 5..9 of the window. Tick 0 is the frame
        // that first carries the request, so the device has had five frames to
        // gather its answer before the first capture, and the last capture is
        // the final frame of the window -- the newest answer the window can
        // hold. Overwriting keeps the latest good copy, so a lost frame in the
        // middle still leaves an earlier one. The final copy is published once,
        // when the window closes.
        if (window_open) {
            if (inputs_ok && service_cycle >= 5 && service_cycle <= 9) {
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
        ++cycle;
    }
}

} // namespace hand_sdk
