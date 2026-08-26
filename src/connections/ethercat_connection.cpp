#include "ethercat_connection.hpp"

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

    // Tell the CoE application to act on SYNC0, while still in PREOP where
    // 0x1C32/0x1C33 are writable (notes.md, "DC-Synchron bring-up").
    if (config_.dc.enabled && config_.dc.write_sync_type) {
        for (int i = 1; i <= context_.slavecount; ++i) {
            if (!isHandSlave(i)) {
                continue;
            }
            writeSyncType(i, 0x1C32, config_.dc.out_sync_type);
            writeSyncType(i, 0x1C33, config_.dc.in_sync_type);
        }
    }

    for (int i = 1; i <= context_.slavecount; ++i) {
        if (isHandSlave(i)) {
            fixInputSmStart(i);
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
    // 0x0981 is volatile, so it is re-armed on every connect; ecx_configdc()
    // above starts no pulse of its own.
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
            ecx_dcsync01(&context_, static_cast<uint16_t>(id), TRUE,
                         config_.dc.sync0_cycle_ns,
                         config_.dc.sync1_shift_from_sync0_ns,
                         config_.dc.sync0_shift_ns);
        }
        dc_active_ = true;

        std::clog << "[EtherCAT] DC-Synchron armed: SYNC0/SYNC1 every "
                  << config_.dc.sync0_cycle_ns / 1000 << " us at +"
                  << config_.dc.sync0_shift_ns / 1000 << " us / +"
                  << config_.dc.sync1_shift_from_sync0_ns / 1000 << " us"
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

    // Station addresses for the direct SM exchange. Logged from the constants,
    // never literals, so the log cannot drift from what exchangeRaw() uses.
    const auto logHand = [this](const char* tag, uint16_t addr) {
        std::clog << "[EtherCAT] " << tag << " hand: configadr=0x" << std::hex << addr
                  << " (direct SM access SM2@0x" << kSmOutAddr << " " << std::dec << kOutBytes
                  << " B / SM3@0x" << std::hex << kSmInAddr << " " << std::dec << kInBytes
                  << " B)" << std::endl;
    };

    if (left_slave_id_ != -1) {
        left_addr_ = context_.slavelist[left_slave_id_].configadr;
        logHand("Left ", left_addr_);
    }
    if (right_slave_id_ != -1) {
        right_addr_ = context_.slavelist[right_slave_id_].configadr;
        logHand("Right", right_addr_);
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
    last_exchange_ok_ = false;   // nothing exchanged yet this session
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
            ecx_dcsync01(&context_, static_cast<uint16_t>(id), FALSE,
                         config_.dc.sync0_cycle_ns,
                         config_.dc.sync1_shift_from_sync0_ns,
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
    last_exchange_ok_ = false;
}

bool EthercatConnection::hasLeft() const
{
    return connected_ && left_slave_id_ != -1;
}

bool EthercatConnection::hasRight() const
{
    return connected_ && right_slave_id_ != -1;
}


bool EthercatConnection::probeOperational(std::string& text)
{
    text.clear();
    if (!connected_) {
        text = "not connected";
        return false;
    }

    bool all_op = true;

    const auto probe = [&](const char* label, int slave_id, uint16_t addr) {
        if (slave_id == -1) {
            return;
        }
        if (!text.empty()) {
            text += " | ";
        }
        text += label;

        // 0x0130 AL status, 0x0132 reserved, 0x0134 AL status code: one
        // six-byte FPRD gets the state and the reason in a single datagram.
        uint8_t buf[6]{};
        const int wkc = ecx_FPRD(&context_.port, addr, static_cast<uint16_t>(ECT_REG_ALSTAT),
                                 static_cast<uint16_t>(sizeof(buf)), buf, EC_TIMEOUTRET);
        if (wkc <= 0) {
            // The slave did not answer a register read either, so this is the
            // wire, not the state machine -- cable, PHY, or the device is gone.
            text += " no answer (link?)";
            all_op = false;
            return;
        }

        uint16_t state = 0;
        uint16_t code  = 0;
        std::memcpy(&state, buf, sizeof(state));
        std::memcpy(&code, buf + 4, sizeof(code));
        state = etohs(state);
        code  = etohs(code);

        text += " " + alStateText(state);
        if (code != 0) {
            std::ostringstream oss;
            oss << " (AL 0x" << std::hex << std::setw(4) << std::setfill('0') << code
                << std::dec << " " << ec_ALstatuscode2string(code) << ")";
            text += oss.str();
        }

        if ((state & 0x0F) != EC_STATE_OPERATIONAL || (state & EC_STATE_ERROR) != 0) {
            all_op = false;
        }
    };

    probe("left", left_slave_id_, left_addr_);
    probe("right", right_slave_id_, right_addr_);

    return all_op;
}

bool EthercatConnection::requestOperational()
{
    if (!connected_) {
        return false;
    }

    bool all_op = true;

    const auto nudge = [&](int slave_id, uint16_t addr) {
        if (slave_id == -1) {
            return;
        }

        uint16_t state = 0;
        if (ecx_FPRD(&context_.port, addr, static_cast<uint16_t>(ECT_REG_ALSTAT),
                     static_cast<uint16_t>(sizeof(state)), &state, EC_TIMEOUTRET) <= 0) {
            // Unreachable. Writing AL control into the dark achieves nothing;
            // wait for the wire and try again next time round.
            all_op = false;
            return;
        }
        state = etohs(state);

        if ((state & 0x0F) == EC_STATE_OPERATIONAL && (state & EC_STATE_ERROR) == 0) {
            return;
        }
        all_op = false;

        // The error latch has to be acknowledged before the slave will accept
        // any state change, so a fault takes two rounds: ACK now, request OP on
        // the next call once the latch has cleared. Same sequence connect()
        // uses; this just runs it while cycling.
        context_.slavelist[slave_id].state = ((state & EC_STATE_ERROR) != 0)
                                                 ? (EC_STATE_SAFE_OP + EC_STATE_ACK)
                                                 : EC_STATE_OPERATIONAL;
        ecx_writestate(&context_, static_cast<uint16_t>(slave_id));
    };

    nudge(left_slave_id_, left_addr_);
    nudge(right_slave_id_, right_addr_);

    return all_op;
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
    last_exchange_ok_ = expected > 0 && wkc >= expected;
    return last_exchange_ok_;
}


int EthercatConnection::exchangeRaw()
{
    int wkc = 0;

    if (dc_active_) {
        // Refresh the DC reference clock to every slave (FRMW) — load-bearing,
        // see notes.md. Not summed into wkc: FRMW counts DC slaves, not hands.
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

bool EthercatConnection::copyInputs(frames::pdo::DualReadPDO& raw)
{
    if (!connected_) {
        return false;
    }

    // A short working counter means at least one slave did not answer this
    // cycle, so left_in_/right_in_ still hold the *previous* frame. Handing
    // that back would publish a stale state and, worse, let a service window
    // capture it as a reply -- which is how a lost frame turns into a
    // well-formed reply of all zeros. Say nothing rather than something wrong.
    if (!last_exchange_ok_) {
        return false;
    }

    raw = {};

    if (left_slave_id_ != -1) {
        std::memcpy(&raw.left, left_in_, sizeof(frames::pdo::ReadPDO));
        raw.has_left = true;
    }

    if (right_slave_id_ != -1) {
        std::memcpy(&raw.right, right_in_, sizeof(frames::pdo::ReadPDO));
        raw.has_right = true;
    }

    return true;
}

bool EthercatConnection::stageOutputs(const frames::pdo::DualWritePDO& raw)
{
    if (!connected_) {
        return false;
    }

    if (raw.has_left && left_slave_id_ != -1) {
        std::memcpy(left_out_, &raw.left, sizeof(frames::pdo::WritePDO));
    }

    if (raw.has_right && right_slave_id_ != -1) {
        std::memcpy(right_out_, &raw.right, sizeof(frames::pdo::WritePDO));
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

bool EthercatConnection::isHandSlave(int slave_id) const
{
    const char* name = context_.slavelist[slave_id].name;
    return std::strcmp(name, config_.left_hand_name.c_str()) == 0 ||
           std::strcmp(name, config_.right_hand_name.c_str()) == 0;
}

void EthercatConnection::fixInputSmStart(int slave_id)
{
    // Some units ship an EEPROM that puts SM3 at 0x1400. Both SMs are 3-buffered,
    // so the outputs at 0x1100 reserve 3 x 260 B up to 0x140B and SM3 lands inside
    // them; the ESC then refuses PREOP -> SAFEOP with AL 0x001D "invalid output
    // configuration". kSmInAddr is the ESI value and leaves no overlap.
    const uint16_t eeprom_start = etohs(context_.slavelist[slave_id].SM[3].StartAddr);
    if (eeprom_start == kSmInAddr) {
        return;
    }

    std::clog << "[EtherCAT]   slave " << slave_id << " EEPROM puts SM3 at 0x"
              << std::hex << eeprom_start << ", overlapping SM2; programming 0x"
              << kSmInAddr << std::dec << " instead" << std::endl;
    context_.slavelist[slave_id].SM[3].StartAddr = htoes(kSmInAddr);
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