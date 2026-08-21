// Scripted bench sequence — read-only apart from two zero-calibrations.
//
//   1. connect
//   2. wait 5 s
//   3. read mc_state
//   4. wait 5 s
//   5. read mc_state again
//   6. wait 5 s
//   7. start zero-calibration on the FIRST joint (j0)
//   8. wait 5 s
//   9. read every joint's calibration status
//  10. start zero-calibration on the SECOND joint (j1)
//  11. disconnect
//
// SAFETY: this program never enables control and never sends a joint command.
// It does not call serviceComm/serviceMode/serviceGains, so no motor is ever
// commanded to hold a position. The two zero-calibration calls DO drive their
// motor: the routine turns the joint looking for its stop. Keep j0 and j1 free
// to move.
//
// Steps 3 and 5 are the same query on purpose — running it twice, five seconds
// apart, is how an intermittent service channel shows itself. A cycle whose
// working counter came up short is reported as a failure rather than as a reply
// of zeros, so "no reply from the targeted hand" here means frames are being
// lost on the wire, not that the device answered with nothing.

#include <hand_sdk.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;
using hand_sdk::service::Hand;
using hand_sdk::service::McState;
using hand_sdk::service::ZeroCalState;

constexpr auto kStep = 5s;

// The two joints this run calibrates, in order.
constexpr std::size_t kFirstJoint  = 0;
constexpr std::size_t kSecondJoint = 1;

std::atomic<bool> g_run{true};
void onSignal(int) { g_run = false; }

// Sleep in 100 ms slices so Ctrl-C lands inside a wait instead of after it.
// Returns false if the run was interrupted.
bool waitStep(const char* why)
{
    std::cout << "  ... waiting "
              << std::chrono::duration_cast<std::chrono::seconds>(kStep).count()
              << " s (" << why << ")" << std::endl;
    const auto until = std::chrono::steady_clock::now() + kStep;
    while (g_run && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(100ms);
    }
    return g_run;
}

const char* mcStateName(McState state)
{
    switch (state) {
    case McState::kNone:    return "none";
    case McState::kInit:    return "init";
    case McState::kStandby: return "standby";
    case McState::kActive:  return "active";
    case McState::kFault:   return "fault";
    }
    return "unknown";
}

// One line per joint that is saying anything, plus a tally. Joints reporting
// `none` are folded into the tally rather than printed: on a partial rig most
// of the twenty-one slots are empty and listing them buries the ones that are
// not.
void printMcState(const hand_sdk::service::McStatusArray& states)
{
    int reporting = 0;
    for (std::size_t j = 0; j < states.size(); ++j) {
        if (states[j].state == McState::kNone) {
            continue;
        }
        ++reporting;
        std::cout << "    j" << std::setw(2) << std::setfill('0') << j << std::setfill(' ')
                  << ": " << mcStateName(states[j].state);
        if (states[j].fault_code != hand_sdk::service::fault::kNone) {
            std::cout << "  fault_code=0x" << std::hex << states[j].fault_code << std::dec;
        }
        std::cout << "\n";
    }
    std::cout << "    " << reporting << "/" << states.size()
              << " joints reporting a state (the rest are `none`)" << std::endl;
}

void printCalib(const hand_sdk::service::ZeroCalStateArray& states)
{
    for (std::size_t j = 0; j < states.size(); ++j) {
        // Every slot is printed here, unlike mc_state: `0 not calibrated` is a
        // real answer for this query, not an empty slot.
        std::cout << "    j" << std::setw(2) << std::setfill('0') << j << std::setfill(' ')
                  << ": " << std::setw(2) << static_cast<int>(states[j])
                  << "  " << hand_sdk::service::stateName(states[j]) << "\n";
    }
    std::cout << std::flush;
}

// Cycle health, printed after every service call. exchange_failures is the
// count of cycles whose working counter came up short -- if it climbs between
// two steps, frames were lost in between and any failure above is the wire's
// doing rather than the device's.
void printStats(hand_sdk::Api& api)
{
    const auto s = api.cycleStats();
    std::cout << "    [cycles " << s.cycles
              << " | exchange failures " << s.exchange_failures
              << " | phase " << s.phase_error_ns / 1000 << " us]" << std::endl;
}

// A read service, run and reported. Returns false only if the run was
// interrupted -- a failed call is printed and the script carries on, since each
// step here is worth seeing independently of whether the last one worked.
bool readMcState(hand_sdk::Api& api, Hand hand, const char* tag)
{
    std::cout << tag << ": serviceMcState()" << std::endl;
    const auto mc = api.serviceMcState(hand);
    if (mc.success) {
        std::cout << "  OK -- " << mc.msg << "\n";
        printMcState(mc.value);
    } else {
        std::cout << "  FAILED -- " << mc.msg << std::endl;
    }
    printStats(api);
    return g_run;
}

// Starts the routine on one joint and nothing else: every other slot carries
// the device's "no action" word. Success means the device took the request --
// the routine itself runs for seconds past the 10 ms service window, so the
// outcome is read later with serviceZeroCalibrateState().
bool calibrateJoint(hand_sdk::Api& api, Hand hand, std::size_t joint, const char* tag)
{
    std::cout << tag << ": serviceZeroCalibrate(j" << joint << ") -- this joint only"
              << std::endl;
    const auto cal = api.serviceZeroCalibrate(
        hand,
        hand_sdk::service::ZeroCalDir::kSameAsEncoder,
        hand_sdk::service::JointMask::single(joint));
    if (cal.success) {
        std::cout << "  OK -- request accepted, calibration started (" << cal.msg << ")\n"
                  << "    j" << joint << " reports "
                  << hand_sdk::service::stateName(cal.value[joint]) << std::endl;
    } else {
        std::cout << "  FAILED -- " << cal.msg << std::endl;
    }
    printStats(api);
    return g_run;
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    hand_sdk::Config config;
    config.interface_name  = "enp132s0";
    config.left_hand_name  = "hand_200_L";
    config.right_hand_name = "hand_200_R";

    // --- 1. connect ---------------------------------------------------------
    auto api = hand_sdk::createEthercat(config);

    std::cout << "1. connect()" << std::endl;
    const auto connected = api.connect();
    if (!connected.success) {
        std::cerr << "  FAILED -- " << connected.msg << std::endl;
        return 1;
    }

    // Every step below addresses one hand. Left when it is there, otherwise
    // right -- the same rule the GUI's readings grid uses.
    const Hand hand = connected.value.has_left ? Hand::kLeft : Hand::kRight;
    const char* hand_tag = connected.value.has_left ? "left" : "right";

    std::cout << "  OK -- left " << (connected.value.has_left ? "yes" : "no")
              << " | right " << (connected.value.has_right ? "yes" : "no")
              << " | DC " << (api.dcActive() ? "armed" : "off") << "\n"
              << "  driving the " << hand_tag << " hand\n"
              << "  left state " << api.leftStatus().state_text
              << " | right state " << api.rightStatus().state_text << std::endl;
    printStats(api);

    // --- 2..5. two mc_state reads, five seconds apart -----------------------
    if (waitStep("settle before the first read") &&
        readMcState(api, hand, "3. mc_state (first)") &&
        waitStep("between the two reads") &&
        readMcState(api, hand, "5. mc_state (second)") &&
        waitStep("before calibrating") &&
        readMcState(api, hand, "6. mc_state (third)") &&
        waitStep("before calibrating") &&

        // --- 6..7. calibrate the first joint --------------------------------
        calibrateJoint(api, hand, kFirstJoint, "7. zero-calibrate first joint") &&
        waitStep("let the routine run")) {

        // --- 9. where every joint's calibration got to ----------------------
        std::cout << "9. serviceZeroCalibrateState() -- all joints" << std::endl;
        const auto cal = api.serviceZeroCalibrateState(hand);
        if (cal.success) {
            std::cout << "  OK -- " << cal.msg << "\n";
            printCalib(cal.value);
        } else {
            std::cout << "  FAILED -- " << cal.msg << std::endl;
        }
        printStats(api);

        // --- 10. calibrate the second joint ---------------------------------
        calibrateJoint(api, hand, kSecondJoint, "10. zero-calibrate second joint");
    }

    if (!g_run) {
        std::cout << "\ninterrupted" << std::endl;
    }

    // --- 11. disconnect -----------------------------------------------------
    // Read the stats before disconnect(): it stops the cyclic thread.
    const auto stats = api.cycleStats();
    api.disconnect();

    // AL state as of disconnect(), which re-reads it. If the exchange died
    // mid-run this is where it says why: a slave that fell out of OP stops
    // acknowledging writes to its output SyncManager, which is exactly what a
    // short working counter on every cycle looks like from the master side.
    const auto& ls = api.leftStatus();
    const auto& rs = api.rightStatus();

    std::cout << "\n11. disconnected\n"
              << "  cycles " << stats.cycles
              << " | exchange failures " << stats.exchange_failures
              << " | phase " << stats.phase_error_ns / 1000 << " us\n";

    const auto printSlave = [](const char* tag, const hand_sdk::SlaveStatus& st) {
        if (!st.present) {
            std::cout << "  " << tag << ": absent\n";
            return;
        }
        std::cout << "  " << tag << ": " << st.state_text
                  << "  (AL status 0x" << std::hex << st.al_state
                  << ", code 0x" << st.al_status_code << std::dec << ")";
        if (!st.error_text.empty()) {
            std::cout << "  -- " << st.error_text;
        }
        std::cout << "\n";
    };
    printSlave("left ", ls);
    printSlave("right", rs);
    std::cout << std::flush;

    return 0;
}
