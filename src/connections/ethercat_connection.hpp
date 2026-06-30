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
    // Direct (FMMU-bypassing) process-data exchange: FPWR command SM + FPRD state SM
    // per slave. Returns summed working counter. See exchangeRaw() definition for why.
    int exchangeRaw();

    // --- Direct SM access constants (workaround for swapped 0x1C00 / FMMUs) ---
    // The slaves describe their SyncManager directions backwards over CoE (0x1C00),
    // so SOEM programs the FMMUs reversed and LRW process data is mis-routed.
    // We bypass logical addressing and talk to the ESC SyncManager regions directly,
    // which are physically correct (verified via ecat_dump live SM registers):
    //   SM2 @ 0x1100, 150 B = master OUTPUT / command (RxPDO)
    //   SM3 @ 0x1400, 180 B = master INPUT  / state   (TxPDO)
    static constexpr uint16_t kSmOutAddr = 0x1100; // command region (master writes)
    static constexpr uint16_t kSmInAddr  = 0x1400; // state region   (master reads)
    static constexpr int      kOutBytes  = 150;
    static constexpr int      kInBytes   = 180;

    Config config_;
    bool connected_{false};

    int left_slave_id_{-1};
    int right_slave_id_{-1};

    // Configured (fixed physical) addresses for FPRD/FPWR addressing.
    uint16_t left_addr_{0};
    uint16_t right_addr_{0};

    // Local process-data buffers owned by us (not the SOEM IOmap).
    uint8_t left_out_[kOutBytes]{};   // command  master → slave (SM2)
    uint8_t left_in_[kInBytes]{};     // state    slave → master (SM3)
    uint8_t right_out_[kOutBytes]{};
    uint8_t right_in_[kInBytes]{};

    // Aliases preserved so readState()/sendCommand() bodies stay unchanged.
    uint8_t* left_tx_{nullptr};    // state  buffer (slave → master) -> left_in_
    uint8_t* left_rx_{nullptr};    // command buffer (master → slave) -> left_out_
    uint8_t* right_tx_{nullptr};
    uint8_t* right_rx_{nullptr};

    static constexpr int kIomapSize = 4096;
    uint8_t iomap_[kIomapSize]{};

    ecx_contextt context_{};
};

} // namespace hand_sdk::connections