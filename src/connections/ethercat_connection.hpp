#pragma once

#include "../frames/messages.hpp"

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <soem/soem.h>
}

namespace hand_sdk::connections
{

class EthercatConnection
{
public:
    explicit EthercatConnection(std::string interface_name, int slave_id = 1);
    ~EthercatConnection();

    bool connect();
    void disconnect();
    bool isConnected() const;

    bool sendCommand(const frames::QCommand& command);
    bool readState(frames::ReadState& state);

    bool readSDO(uint16_t index, uint8_t subindex, std::vector<uint8_t>& data, int size);
    bool writeSDO(uint16_t index, uint8_t subindex, const std::vector<uint8_t>& data);

    bool updatePDO();

private:
    std::string interface_name_;
    int slave_id_{1};
    bool connected_{false};

    uint8_t* input_pdo_{nullptr};   // TxPDO: slave -> master
    uint8_t* output_pdo_{nullptr};  // RxPDO: master -> slave

    ecx_contextt context_{};

    static constexpr int kIomapSize = 4096;
    uint8_t iomap_[kIomapSize]{};
};

} // namespace hand_sdk::connections