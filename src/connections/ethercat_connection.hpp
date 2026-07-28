#pragma once

#include "../../include/config.hpp"
#include "../../include/status.hpp"
#include "../../include/messages.hpp"
#include "../frames/pdo_layout.hpp"

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

    // Exchange the process-data buffers on the wire. Driven by CyclicTask on the
    // DC grid; sendCommand() only fills the output buffer and readState() only
    // decodes the input buffer — neither touches the network.
    bool update();

    bool readState(frames::DualHandState& state);
    bool sendCommand(const frames::DualHandCommand& command);

    // --- Distributed Clocks ------------------------------------------------

    // System time (ns) of the DC reference clock, resampled by every update().
    // Phase-lock the cyclic task against this; see hand_sdk::rt::CyclicTimer.
    int64_t dcTime() const { return dc_time_; }

    // True once the SYNC0/SYNC1 cyclic units have been armed on the hands.
    bool dcActive() const { return dc_active_; }

    int leftSlaveId() const { return left_slave_id_; }
    int rightSlaveId() const { return right_slave_id_; }

    // The ESC's own verdict on whether we are meeting the SYNC deadlines.
    // Read over CoE, so NOT for the cyclic path — mailbox round-trips take
    // milliseconds and will blow the cycle budget.
    SyncDiagnostics syncDiagnostics(int slave_id);

    // AL state captured at the end of connect() and refreshed by disconnect().
    const SlaveStatus& leftStatus() const { return left_status_; }
    const SlaveStatus& rightStatus() const { return right_status_; }

private:
    // Read every slave's AL state off the bus and cache it into left/right
    // status. Bus traffic — only safe when the cyclic thread is not running.
    void captureStatus();

    // Log one line per slave: current AL state and any latched error.
    void logBusState(const char* stage);

    // Direct (FMMU-bypassing) process-data exchange: FPWR command SM + FPRD state SM
    // per slave. Returns summed working counter. See exchangeRaw() definition for why.
    int exchangeRaw();

    // Write 0x1C32:01 / 0x1C33:01 (SyncManager sync type) and log the readback.
    // PREOP only; failures are logged, not fatal.
    void writeSyncType(int slave_id, uint16_t index, uint16_t sync_type);

    // --- Direct SM access constants (workaround for swapped 0x1C00 / FMMUs) ---
    // The slaves were observed to describe their SyncManager directions backwards
    // over CoE (0x1C00), so SOEM programs the FMMUs reversed and LRW process data
    // is mis-routed. We bypass logical addressing and talk to the ESC SyncManager
    // regions directly. Addresses come from the device ESI:
    //   <Sm DefaultSize="260" StartAddress="#x1100" ControlByte="#x64">Outputs</Sm>
    //   <Sm DefaultSize="260" StartAddress="#x1410" ControlByte="#x20">Inputs</Sm>
    //
    // 0x1410 is not arbitrary: both SMs are 3-buffer mode (ControlByte bits 0-1 =
    // 00), so Outputs reserves 3 x 260 = 780 B spanning 0x1100..0x140B, and Inputs
    // starts at the next aligned address above that. Addressing inputs at 0x1400
    // would read back part of the third *output* buffer.
    //
    // NOTE: the ESI shipped with these devices now lists 0x1C00 as 1,2,3,4 — the
    // correct, un-swapped order. If the firmware agrees, this whole direct-SM
    // detour can go away in favour of ecx_send_processdata(). Confirm with
    // ecat_dump against live registers before changing it.
    static constexpr uint16_t kSmOutAddr = 0x1100; // command region (master writes)
    static constexpr uint16_t kSmInAddr  = 0x1410; // state region   (master reads)

    // Tied to the PDO structs so the copy bounds and the wire sizes cannot drift
    // apart; pdo_layout.hpp asserts both against the ESI's 260-byte SyncManagers.
    static constexpr int kOutBytes = static_cast<int>(sizeof(frames::pdo::WritePDO));
    static constexpr int kInBytes  = static_cast<int>(sizeof(frames::pdo::ReadPDO));

    Config config_;
    bool connected_{false};

    int left_slave_id_{-1};
    int right_slave_id_{-1};

    // Configured (fixed physical) addresses for FPRD/FPWR addressing.
    uint16_t left_addr_{0};
    uint16_t right_addr_{0};

    // Station address of the DC reference clock, elected by ecx_configdc().
    uint16_t dc_ref_addr_{0};
    // Last system time read from that reference clock (ns, EtherCAT epoch).
    int64_t dc_time_{0};
    bool dc_active_{false};

    // Local process-data buffers owned by us (not the SOEM IOmap). Touched only
    // by the cyclic thread, so they need no synchronisation of their own — the
    // handoff to other threads happens in CyclicTask's holders.
    uint8_t left_out_[kOutBytes]{};   // command  master → slave (SM2)
    uint8_t left_in_[kInBytes]{};     // state    slave → master (SM3)
    uint8_t right_out_[kOutBytes]{};
    uint8_t right_in_[kInBytes]{};

    // Written by ecx_config_map_group() and then never read: we address the
    // SyncManagers directly rather than through logical addressing. The call is
    // still required — it is what configures the SMs on the slaves — so the
    // buffer has to exist even though the IOmap it produces goes unused.
    SlaveStatus left_status_{};
    SlaveStatus right_status_{};

    static constexpr int kIomapSize = 4096;
    uint8_t iomap_[kIomapSize]{};

    ecx_contextt context_{};
};

} // namespace hand_sdk::connections