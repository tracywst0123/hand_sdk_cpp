#pragma once

#include "../include/service.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// Step two of the write funnel, and the words it says.
//
// A write service succeeds in two stages. Step one is transport: a reply block
// came back for this hand and its cmd id is the one that was sent. That is all
// the read services check, and all they can check -- nothing was asked to
// change, so there is nothing to hold anyone to.
//
// Step two is per joint, and lives here. The device echoes its resulting state
// in the same slots the request was written into, so every joint the request
// addressed can be held to what it was told: exactly for the enum writes (comm,
// mode), within Config::service_tolerance for gains. One addressed joint
// disagreeing fails the call, and the message names the joints on both sides
// with what came back.
//
// Joints outside the mask are not looked at. They were sent the device's "no
// action" word and were never asked for anything, so they can neither pass nor
// fail.

namespace hand_sdk::service::report
{

// "0-5,9,17-20" rather than eleven comma-separated numbers. Runs of three or
// more collapse to a range; a pair stays spelled out, since "3,4" is no longer
// than "3-4" and reads without a rule.
inline std::string jointList(const std::vector<std::size_t>& joints)
{
    if (joints.empty()) {
        return "-";
    }

    std::string out;
    for (std::size_t i = 0; i < joints.size();) {
        std::size_t end = i;
        while (end + 1 < joints.size() && joints[end + 1] == joints[end] + 1) {
            ++end;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += std::to_string(joints[i]);
        if (end > i) {
            out += (end == i + 1 ? "," : "-") + std::to_string(joints[end]);
        }
        i = end + 1;
    }
    return out;
}

inline std::string hexWord(uint16_t value)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(4) << std::setfill('0') << value;
    return oss.str();
}

inline std::string number(float value)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4) << value;
    return oss.str();
}

struct JointCheck
{
    bool ok{false};
    std::string msg;
};

// `matches` decides one joint; `describe` renders what came back for the ones
// that did not.
template <typename Match, typename Describe>
JointCheck checkJoints(const char* what, const JointMask& joints,
                       Match matches, Describe describe)
{
    // Nothing left to hold to account. Reached when a request addressed only
    // slots with no motor behind them, which is a caller mistake rather than a
    // silent pass.
    if (!joints.any()) {
        return {false, std::string(what) + ": the request addressed no motor"};
    }

    std::vector<std::size_t> passed;
    std::vector<std::string> failed;
    for (std::size_t j = 0; j < JointMask::kSize; ++j) {
        if (!joints.test(j)) {
            continue;
        }
        if (matches(j)) {
            passed.push_back(j);
        } else {
            failed.push_back("j" + std::to_string(j) + " " + describe(j));
        }
    }

    std::string msg = std::string(what) + ": " + std::to_string(passed.size()) + "/" +
                      std::to_string(joints.count()) + " joints took it | pass: " +
                      jointList(passed);
    if (!failed.empty()) {
        msg += " | fail: ";
        for (std::size_t i = 0; i < failed.size(); ++i) {
            msg += (i == 0 ? "" : ", ") + failed[i];
        }
    }
    return {failed.empty(), msg};
}

} // namespace hand_sdk::service::report
