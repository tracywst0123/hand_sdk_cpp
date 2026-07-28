#include "ethercat_connection.hpp"

#include "../frames/pdo_codec.hpp"
#include "../frames/pdo_layout.hpp"

extern "C" {
#include <soem/soem.h>
}

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace hand_sdk::connections
{
namespace
{

// Render an AL status register as text. Low nibble is the state, bit 4 the
// error latch — so 0x14 is "SAFE_OP+ERROR", the classic "device refused OP".
std::string alStateText(uint16_t state)
{
    std::string text;
    switch (state & 0x0F) {
        case EC_STATE_INIT:        text = "INIT";    break;
        case EC_STATE_PRE_OP:      text = "PRE_OP";  break;
        case EC_STATE_BOOT:        text = "BOOT";    break;
        case EC_STATE_SAFE_OP:     text = "SAFE_OP"; break;
        case EC_STATE_OPERATIONAL: text = "OP";      break;
        case EC_STATE_NONE:        text = "NONE";    break;
        default: {
            std::ostringstream oss;
            oss << "0x" << std::hex << (state & 0x0F);
            text = oss.str();
            break;
        }
    }
    if ((state & EC_STATE_ERROR) != 0) {
        text += "+ERROR";
    }
    return text;
}

} // namespace

EthercatConnection::EthercatConnection(Config config)
    : config_(std::move(config))
{
}

EthercatConnection::~EthercatConnection()
{
    disconnect();
}

std::string EthercatConnection::connect()
{
    if (connected_) {
        return {};
    }

    std::clog << "[EtherCAT] ecx_init on '" << config_.interface_name << "'..." << std::endl;
    if (!ecx_init(&context_, config_.interface_name.c_str())) {
        return "ecx_init failed: could not open interface '" + config_.interface_name + "'";
    }

    std::clog << "[EtherCAT] ecx_config_init (scanning bus)..." << std::endl;
    if (ecx_config_init(&context_) <= 0) {
        ecx_close(&context_);
        return "ecx_config_init failed: no EtherCAT slaves found on '" + config_.interface_name + "'";
    }
    std::clog << "[EtherCAT] found " << context_.slavecount << " slave(s):" << std::endl;
    for (int i = 1; i <= context_.slavecount; ++i) {
        std::clog << "  [" << i << "] '" << context_.slavelist[i].name << "'" << std::endl;
    }

    // Switch the CoE application to DC mode while we are still in PREOP, where
    // 0x1C32/0x1C33 are writable. Arming the ESC pulses further down is not
    // necessarily enough on its own — the application also has to be told to act
    // on SYNC0/SYNC1 instead of on SyncManager events.
    if (config_.dc.enabled && config_.dc.write_sync_type) {
        for (int i = 1; i <= context_.slavecount; ++i) {
            const bool is_hand =
                std::strcmp(context_.slavelist[i].name, config_.left_hand_name.c_str()) == 0 ||
                std::strcmp(context_.slavelist[i].name, config_.right_hand_name.c_str()) == 0;
            if (!is_hand) {
                continue;
            }
            writeSyncType(i, 0x1C32, config_.dc.out_sync_type);
            writeSyncType(i, 0x1C33, config_.dc.in_sync_type);
        }
    }

    ecx_config_map_group(&context_, iomap_, 0);
    ecx_configdc(&context_);

    // Identify left and right hand slaves by name
    left_slave_id_  = -1;
    right_slave_id_ = -1;

    for (int i = 1; i <= context_.slavecount; ++i) {
        if (std::strcmp(context_.slavelist[i].name, config_.left_hand_name.c_str()) == 0) {
            left_slave_id_ = i;
        } else if (std::strcmp(context_.slavelist[i].name, config_.right_hand_name.c_str()) == 0) {
            right_slave_id_ = i;
        }
    }

    if (left_slave_id_ == -1 && right_slave_id_ == -1) {
        std::string found;
        for (int i = 1; i <= context_.slavecount; ++i) {
            found += std::string(" '") + context_.slavelist[i].name + "'";
        }
        ecx_close(&context_);
        return "no matching hand slaves found (looking for '" + config_.left_hand_name +
               "' / '" + config_.right_hand_name + "'); slaves on bus:" +
               (found.empty() ? " none" : found);
    }

    // --- Arm the DC cyclic unit (DC-Synchron) -------------------------------
    // The hands' ESI declares DC-Synchron, but the cyclic-unit activation
    // register (0x0981) is volatile: whatever a previous master armed is gone
    // after a power cycle. So we arm it here on every connect.
    //
    // ecx_configdc() above only elected the reference clock and programmed the
    // per-slave propagation-delay offsets — it does not start any SYNC pulse.
    if (config_.dc.enabled) {
        if (!context_.slavelist[0].hasdc) {
            ecx_close(&context_);
            return "DC-Synchron requested but no DC-capable slave found on the bus";
        }

        // slavelist[0].DCnext is the first DC-capable slave: the reference clock.
        dc_ref_addr_ = context_.slavelist[context_.slavelist[0].DCnext].configadr;

        for (const int id : {left_slave_id_, right_slave_id_}) {
            if (id == -1) {
                continue;
            }
            // SYNC0 only: ecx_dcsync0() writes 0x0981 = 0x03, exactly the
            // activation the ESI's <AssignActivate>#x300</AssignActivate> asks
            // for. ecx_dcsync01() would write 0x07 and enable a SYNC1 this
            // device does not implement (0x1C32:04 bit 3 clear).
            ecx_dcsync0(&context_, static_cast<uint16_t>(id), TRUE,
                        config_.dc.sync0_cycle_ns,
                        config_.dc.sync0_shift_ns);
        }
        dc_active_ = true;

        std::clog << "[EtherCAT] DC-Synchron armed: SYNC0 every "
                  << config_.dc.sync0_cycle_ns / 1000 << " us at +"
                  << config_.dc.sync0_shift_ns / 1000 << " us"
                  << ", reference clock 0x" << std::hex << dc_ref_addr_ << std::dec
                  << std::endl;
        // SOEM schedules the first pulse ~100 ms out (its SyncDelay), so the
        // pulses come alive during the OP loop below while the PLLs settle.
    }

    logBusState("DC setup");

    // ecx_config_map_group() already requested SAFE_OP; this only waits for it.
    context_.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context_, 0);
    ecx_statecheck(&context_, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    logBusState("SAFE_OP request");

    std::clog << "[EtherCAT] requesting OPERATIONAL..." << std::endl;
    context_.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context_, 0);

    // Station addresses for the FPWR/FPRD addressing exchangeRaw() uses to reach
    // the (physically correct) SyncManager regions directly.
    if (left_slave_id_ != -1) {
        left_addr_ = context_.slavelist[left_slave_id_].configadr;
        std::clog << "[EtherCAT] Left hand: configadr=0x" << std::hex << left_addr_ << std::dec
                  << " (direct SM access SM2@0x1100/SM3@0x1400)" << std::endl;
    }
    if (right_slave_id_ != -1) {
        right_addr_ = context_.slavelist[right_slave_id_].configadr;
        std::clog << "[EtherCAT] Right hand: configadr=0x" << std::hex << right_addr_ << std::dec
                  << " (direct SM access SM2@0x1100/SM3@0x1400)" << std::endl;
    }

    bool op_ready = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        // Direct SM exchange instead of ecx_send/receive_processdata (LRW),
        // since the FMMUs are programmed reversed (see exchangeRaw()).
        exchangeRaw();
        ecx_readstate(&context_);

        // Keep nudging slaves towards OP. Some devices park in SAFE_OP until
        // they receive process data cycles and explicit OP requests.
        for (int i = 1; i <= context_.slavecount; ++i) {
            const auto slave = static_cast<uint16_t>(i);
            if (context_.slavelist[i].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
                context_.slavelist[i].state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                ecx_writestate(&context_, slave);
            } else if (context_.slavelist[i].state == EC_STATE_SAFE_OP) {
                context_.slavelist[i].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context_, slave);
            }
        }

        ecx_statecheck(&context_, 0, EC_STATE_OPERATIONAL, 50000); // 50 ms per attempt

        const bool left_ok = (left_slave_id_ == -1) ||
            (context_.slavelist[left_slave_id_].state == EC_STATE_OPERATIONAL);
        const bool right_ok = (right_slave_id_ == -1) ||
            (context_.slavelist[right_slave_id_].state == EC_STATE_OPERATIONAL);

        if (left_ok && right_ok) {
            op_ready = true;
            break;
        }
    }
    logBusState("OP request");

    if (!op_ready) {
        std::ostringstream oss;
        oss << "slaves found but failed to reach OPERATIONAL state";
        for (int i = 1; i <= context_.slavecount; ++i) {
            const auto& slave = context_.slavelist[i];
            oss << "\n  [" << i << "] '" << slave.name
                << "' state=0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(slave.state)
                << " al=0x" << std::setw(4) << static_cast<int>(slave.ALstatuscode)
                << std::dec << " (" << ec_ALstatuscode2string(slave.ALstatuscode) << ")";
        }
        if (dc_active_) {
            oss << "\n  (DC-Synchron was armed; AL 0x001A/0x0032 point at sync or PLL"
                   " trouble, 0x0030 at a DC configuration the device rejects)";
        }
        ecx_close(&context_);
        dc_active_   = false;
        dc_ref_addr_ = 0;
        dc_time_     = 0;
        return oss.str();
    }

    connected_ = true;
    captureStatus();
    return {};
}

void EthercatConnection::disconnect()
{
    if (!connected_) {
        return;
    }

    // Snapshot where the bus ended up before tearing anything down — if a hand
    // dropped out of OP during the run, this is the only record of why.
    captureStatus();
    logBusState("run");

    // Disarm the cyclic units first: once we stop cycling, nothing refreshes the
    // reference clock, and we would leave the hands firing SYNC0 off a
    // free-running clock.
    if (dc_active_) {
        for (const int id : {left_slave_id_, right_slave_id_}) {
            if (id == -1) {
                continue;
            }
            ecx_dcsync0(&context_, static_cast<uint16_t>(id), FALSE,
                        config_.dc.sync0_cycle_ns,
                        config_.dc.sync0_shift_ns);
        }
        dc_active_ = false;
    }

    context_.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context_, 0);
    ecx_close(&context_);

    dc_ref_addr_ = 0;
    dc_time_     = 0;

    left_addr_  = 0;
    right_addr_ = 0;
    left_slave_id_  = -1;
    right_slave_id_ = -1;
    connected_ = false;
}

bool EthercatConnection::hasLeft() const
{
    return connected_ && left_slave_id_ != -1;
}

bool EthercatConnection::hasRight() const
{
    return connected_ && right_slave_id_ != -1;
}


bool EthercatConnection::update()
{
    if (!connected_) {
        return false;
    }

    int wkc = exchangeRaw();
    int expected = 0;
    if (left_slave_id_  != -1) expected += 2;
    if (right_slave_id_ != -1) expected += 2;
    return expected > 0 && wkc >= expected;
}


int EthercatConnection::exchangeRaw()
{
    int wkc = 0;

    if (dc_active_) {
        // Read the reference clock and broadcast it to every slave (FRMW), so
        // each ESC PLL keeps correcting its own crystal drift. SOEM normally
        // does this inside ecx_send_processdata_group(), which we bypass — and
        // without it the SYNC0 pulses walk off our cycle within seconds and the
        // hands fault with a sync/PLL error.
        //
        // Deliberately not summed into wkc: FRMW's working counter counts DC
        // slaves on the bus, not our two hands.
        int64_t dct = 0;
        (void)ecx_FRMW(&context_.port, dc_ref_addr_,
                       static_cast<uint16_t>(ECT_REG_DCSYSTIME),
                       static_cast<uint16_t>(sizeof(dct)), &dct, EC_TIMEOUTRET);
        dc_time_ = etohll(dct);
    }

    if (left_slave_id_ != -1) {
        wkc += ecx_FPWR(&context_.port, left_addr_, kSmOutAddr, kOutBytes, left_out_, EC_TIMEOUTRET);
        wkc += ecx_FPRD(&context_.port, left_addr_, kSmInAddr,  kInBytes,  left_in_,  EC_TIMEOUTRET);
    }
    if (right_slave_id_ != -1) {
        wkc += ecx_FPWR(&context_.port, right_addr_, kSmOutAddr, kOutBytes, right_out_, EC_TIMEOUTRET);
        wkc += ecx_FPRD(&context_.port, right_addr_, kSmInAddr,  kInBytes,  right_in_,  EC_TIMEOUTRET);
    }

    return wkc;
}

bool EthercatConnection::readState(frames::DualHandState& state)
{
    if (!connected_) {
        return false;
    }

    state = {};

    if (left_slave_id_ != -1) {
        frames::pdo::ReadPDO pdo{};
        std::memcpy(&pdo, left_in_, sizeof(frames::pdo::ReadPDO));
        state.left     = frames::pdo::decodeReadPDO(pdo);
        state.has_left = true;
    }

    if (right_slave_id_ != -1) {
        frames::pdo::ReadPDO pdo{};
        std::memcpy(&pdo, right_in_, sizeof(frames::pdo::ReadPDO));
        state.right     = frames::pdo::decodeReadPDO(pdo);
        state.has_right = true;
    }

    return true;
}

bool EthercatConnection::sendCommand(const frames::DualHandCommand& command)
{
    if (!connected_) {
        return false;
    }

    if (command.has_left && left_slave_id_ != -1) {
        auto pdo = frames::pdo::encodeWritePDO(command.left);
        std::memcpy(left_out_, &pdo, sizeof(frames::pdo::WritePDO));
    }

    if (command.has_right && right_slave_id_ != -1) {
        auto pdo = frames::pdo::encodeWritePDO(command.right);
        std::memcpy(right_out_, &pdo, sizeof(frames::pdo::WritePDO));
    }

    return true;
}

void EthercatConnection::logBusState(const char* stage)
{
    ecx_readstate(&context_);
    std::clog << "[EtherCAT] state after " << stage << ":";
    for (int i = 1; i <= context_.slavecount; ++i) {
        const auto& s = context_.slavelist[i];
        std::clog << "  [" << i << "] " << alStateText(s.state);
        if ((s.state & EC_STATE_ERROR) != 0 || s.ALstatuscode != 0) {
            std::clog << " (AL 0x" << std::hex << std::setw(4) << std::setfill('0')
                      << s.ALstatuscode << std::dec << " "
                      << ec_ALstatuscode2string(s.ALstatuscode) << ")";
        }
    }
    std::clog << std::endl;
}

void EthercatConnection::captureStatus()
{
    ecx_readstate(&context_);

    const auto fill = [&](int slave_id, SlaveStatus& out) {
        out = {};
        if (slave_id < 1 || slave_id > context_.slavecount) {
            return;
        }
        const auto& s = context_.slavelist[slave_id];
        out.present        = true;
        out.name           = s.name;
        out.al_state       = s.state;
        out.al_status_code = s.ALstatuscode;
        out.state_text     = alStateText(s.state);
        out.error_text     = ec_ALstatuscode2string(s.ALstatuscode);
    };

    fill(left_slave_id_, left_status_);
    fill(right_slave_id_, right_status_);
}

void EthercatConnection::writeSyncType(int slave_id, uint16_t index, uint16_t sync_type)
{
    const auto slave = static_cast<uint16_t>(slave_id);

    if (ecx_SDOwrite(&context_, slave, index, 0x01, FALSE,
                     static_cast<int>(sizeof(sync_type)), &sync_type, EC_TIMEOUTRXM) <= 0) {
        std::clog << "[EtherCAT]   slave " << slave_id << " 0x" << std::hex << index
                  << std::dec << ":01 write rejected (sync type may be fixed in firmware)"
                  << std::endl;
    }

    // Report what the device settled on either way — that, not our write, is
    // what decides whether it honours the SYNC pulses.
    uint16_t readback = 0;
    int size = static_cast<int>(sizeof(readback));
    if (ecx_SDOread(&context_, slave, index, 0x01, FALSE, &size, &readback, EC_TIMEOUTRXM) > 0) {
        std::clog << "[EtherCAT]   slave " << slave_id << " 0x" << std::hex << index
                  << ":01 sync type = 0x" << readback << std::dec
                  << (readback == sync_type ? " (as requested)" : " (NOT as requested)")
                  << std::endl;
    }
}

SyncDiagnostics EthercatConnection::syncDiagnostics(int slave_id)
{
    SyncDiagnostics diag{};
    if (!connected_ || slave_id < 1 || slave_id > context_.slavecount) {
        return diag;
    }

    // 0x1C32 = output (RxPDO) SyncManager parameters, 0x1C33 = input (TxPDO).
    const auto read16 = [&](uint16_t index, uint8_t sub, uint16_t& out) {
        uint16_t value = 0;
        int size = static_cast<int>(sizeof(value));
        const int wkc = ecx_SDOread(&context_, static_cast<uint16_t>(slave_id),
                                    index, sub, FALSE, &size, &value, EC_TIMEOUTRXM);
        if (wkc <= 0) {
            return false;
        }
        out = value;
        return true;
    };

    // Sync types are mandatory when the device supports these objects at all;
    // the error counters are optional, so their absence is not a failure.
    const bool ok = read16(0x1C32, 0x01, diag.out_sync_type) &&
                    read16(0x1C33, 0x01, diag.in_sync_type);

    read16(0x1C32, 0x0B, diag.out_cycle_exceeded);
    read16(0x1C33, 0x0B, diag.in_cycle_exceeded);
    read16(0x1C32, 0x0C, diag.out_shift_too_short);
    read16(0x1C33, 0x0C, diag.in_shift_too_short);

    diag.valid = ok;
    return diag;
}

} // namespace hand_sdk::connections