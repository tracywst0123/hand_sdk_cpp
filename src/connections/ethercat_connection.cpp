#include "ethercat_connection.hpp"

#include "../frames/pdo_codec.hpp"
#include "../frames/pdo_layout.hpp"

extern "C" {
#include <soem/soem.h>
}

#include <cstring>
#include <utility>

namespace hand_sdk::connections
{

EthercatConnection::EthercatConnection(std::string interface_name, int slave_id)
    : interface_name_(std::move(interface_name)), slave_id_(slave_id)
{
}

EthercatConnection::~EthercatConnection()
{
    disconnect();
}

bool EthercatConnection::connect()
{
    if (!ecx_init(&context_, interface_name_.c_str())) {
        return false;
    }

    if (ecx_config_init(&context_) <= 0) {
        ecx_close(&context_);
        return false;
    }

    ecx_config_map_group(&context_, iomap_, 0);
    ecx_configdc(&context_);

    context_.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context_, 0);
    ecx_statecheck(&context_, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);

    input_pdo_ = context_.slavelist[slave_id_].inputs;
    output_pdo_ = context_.slavelist[slave_id_].outputs;

    if (input_pdo_ == nullptr || output_pdo_ == nullptr) {
        ecx_close(&context_);
        return false;
    }

    context_.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context_, 0);

    for (int i = 0; i < 100; ++i) {
        ecx_send_processdata(&context_);
        ecx_receive_processdata(&context_, EC_TIMEOUTRET);
        ecx_statecheck(&context_, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);

        if (context_.slavelist[0].state == EC_STATE_OPERATIONAL) {
            connected_ = true;
            return true;
        }
    }

    ecx_close(&context_);
    return false;
}

void EthercatConnection::disconnect()
{
    if (!connected_) {
        return;
    }

    context_.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context_, 0);
    ecx_close(&context_);

    input_pdo_ = nullptr;
    output_pdo_ = nullptr;
    connected_ = false;
}

bool EthercatConnection::isConnected() const
{
    return connected_;
}

bool EthercatConnection::updatePDO()
{
    if (!connected_) {
        return false;
    }

    ecx_send_processdata(&context_);
    int wkc = ecx_receive_processdata(&context_, EC_TIMEOUTRET);

    return wkc > 0;
}

bool EthercatConnection::sendCommand(const frames::QCommand& command)
{
    if (!connected_ || output_pdo_ == nullptr) {
        return false;
    }

    auto pdo = frames::pdo::encodeRxPDO(command);
    std::memcpy(output_pdo_, &pdo, sizeof(frames::pdo::RxPDO));

    return true;
}

bool EthercatConnection::readState(frames::ReadState& state)
{
    if (!connected_ || input_pdo_ == nullptr) {
        return false;
    }

    frames::pdo::TxPDO pdo{};
    std::memcpy(&pdo, input_pdo_, sizeof(frames::pdo::TxPDO));

    state = frames::pdo::decodeTxPDO(pdo);

    return true;
}

bool EthercatConnection::readSDO(uint16_t index, uint8_t subindex, std::vector<uint8_t>& data, int size)
{
    if (!connected_) {
        return false;
    }

    data.resize(size);
    int read_size = size;

    int wkc = ecx_SDOread(
        &context_,
        slave_id_,
        index,
        subindex,
        FALSE,
        &read_size,
        data.data(),
        EC_TIMEOUTRXM
    );

    if (wkc <= 0) {
        data.clear();
        return false;
    }

    data.resize(read_size);
    return true;
}

bool EthercatConnection::writeSDO(uint16_t index, uint8_t subindex, const std::vector<uint8_t>& data)
{
    if (!connected_) {
        return false;
    }

    int wkc = ecx_SDOwrite(
        &context_,
        slave_id_,
        index,
        subindex,
        FALSE,
        static_cast<int>(data.size()),
        const_cast<uint8_t*>(data.data()),
        EC_TIMEOUTRXM
    );

    return wkc > 0;
}

} // namespace hand_sdk::connections