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

    std::clog << "[EtherCAT] transitioning to SAFE_OP..." << std::endl;
    context_.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context_, 0);
    ecx_statecheck(&context_, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    std::clog << "[EtherCAT] SAFE_OP reached, transitioning to OPERATIONAL..." << std::endl;

    context_.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context_, 0);

    // exchangeRaw() moves readState()/sendCommand() with direct FPWR/FPRD to the (physically correct)
    // SyncManager regions SM2@0x1100 (command) / SM3@0x1400 (state).
    if (left_slave_id_ != -1) {
        left_addr_ = context_.slavelist[left_slave_id_].configadr;
        left_rx_   = left_out_;   // command  (master → slave, SM2)
        left_tx_   = left_in_;    // state    (slave → master, SM3)
        std::clog << "[EtherCAT] Left hand: configadr=0x" << std::hex << left_addr_ << std::dec
                  << " (direct SM access SM2@0x1100/SM3@0x1400)" << std::endl;
    }
    if (right_slave_id_ != -1) {
        right_addr_ = context_.slavelist[right_slave_id_].configadr;
        right_rx_   = right_out_;
        right_tx_   = right_in_;
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
            if (context_.slavelist[i].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
                context_.slavelist[i].state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                ecx_writestate(&context_, i);
            } else if (context_.slavelist[i].state == EC_STATE_SAFE_OP) {
                context_.slavelist[i].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context_, i);
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
    std::clog << "[EtherCAT] OPERATIONAL mode on" << std::endl;

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
        ecx_close(&context_);
        return oss.str();
    }

    connected_ = true;
    return {};
}

void EthercatConnection::disconnect()
{
    if (!connected_) {
        return;
    }

    context_.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context_, 0);
    ecx_close(&context_);

    left_tx_  = nullptr;
    left_rx_  = nullptr;
    right_tx_ = nullptr;
    right_rx_ = nullptr;
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

    if (left_slave_id_ != -1 && left_tx_ != nullptr) {
        frames::pdo::ReadPDO pdo{};
        std::memcpy(&pdo, left_tx_, sizeof(frames::pdo::ReadPDO));
        state.left     = frames::pdo::decodeReadPDO(pdo);
        state.has_left = true;
    }

    if (right_slave_id_ != -1 && right_tx_ != nullptr) {
        frames::pdo::ReadPDO pdo{};
        std::memcpy(&pdo, right_tx_, sizeof(frames::pdo::ReadPDO));
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

    if (command.has_left && left_slave_id_ != -1 && left_rx_ != nullptr) {
        auto pdo = frames::pdo::encodeWritePDO(command.left);
        std::memcpy(left_rx_, &pdo, sizeof(frames::pdo::WritePDO));
    }

    if (command.has_right && right_slave_id_ != -1 && right_rx_ != nullptr) {
        auto pdo = frames::pdo::encodeWritePDO(command.right);
        std::memcpy(right_rx_, &pdo, sizeof(frames::pdo::WritePDO));
    }

    return true;
}

std::string EthercatConnection::name() const
{
    return "EtherCAT:" + config_.interface_name;
}

} // namespace hand_sdk::connections