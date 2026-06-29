#pragma once

#include "../../include/config.hpp"
#include "../frames/messages.hpp"

#include <cstdint>
#include <string>

extern "C" {
#include <soem/soem.h>
}

namespace hand_sdk::connections
{

class EthercatConnection
{
public:
    explicit EthercatConnection(Config config);
    ~EthercatConnection();

    // Returns empty string on success, error description on failure.
    std::string connect();
    void disconnect();

    bool hasLeft() const;
    bool hasRight() const;

    // Exchange PDO buffers over the network. Call at a fixed rate (e.g. 200 Hz).
    // sendCommand() writes to the output buffer; readState() reads from the input buffer.
    // Neither transmits on the wire — only update() does.
    bool update();

    bool readState(frames::DualHandState& state);
    bool sendCommand(const frames::DualHandCommand& command);

    std::string name() const;

private:
    bool exchangePDO();

    Config config_;
    bool connected_{false};

    int left_slave_id_{-1};
    int right_slave_id_{-1};

    uint8_t* left_tx_{nullptr};    // ReadPDO: slave → master (state)
    uint8_t* left_rx_{nullptr};    // WritePDO: master → slave (commands)
    uint8_t* right_tx_{nullptr};
    uint8_t* right_rx_{nullptr};

    static constexpr int kIomapSize = 4096;
    uint8_t iomap_[kIomapSize]{};

    ecx_contextt context_{};
};

} // namespace hand_sdk::connections