// Standalone bit-IO read test for verifying the SOEM bring-up pipeline.
//
// This deliberately does NOT use the hand_sdk EthercatConnection wrapper or the
// 21-joint PDO layout. It talks to SOEM directly so you can confirm that
// init -> config_init -> config_map -> SAFE_OP -> OP -> process-data exchange
// works against a simple device whose process data is just bits (e.g. the
// SSC-IO sample device: 8 DI switches in, 8 DO LEDs out).
//
// It reads slave.inputs and prints every input bit channel each cycle, so you
// can toggle a physical switch and watch the corresponding bit change.
//
// Build: produced as `bit_io_test` by CMakeLists.txt.
// Run (needs raw socket access):
//     sudo ./bit_io_test <interface>
//     e.g. sudo ./bit_io_test enp132s0
//
// Assumes exactly one slave on the bus.

#include "bit_device.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

extern "C" {
#include <soem/soem.h>
}

namespace {
std::atomic<bool> g_run{true};
void onSignal(int) { g_run = false; }
} // namespace

int main(int argc, char** argv)
{
    const std::string iface = (argc > 1) ? argv[1] : "enp132s0";

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    ecx_contextt context{};
    static uint8_t iomap[4096]{};

    std::cout << "[bit_io] ecx_init on '" << iface << "'..." << std::endl;
    if (!ecx_init(&context, iface.c_str())) {
        std::cerr << "ecx_init failed: could not open interface '" << iface
                  << "'. Need root / CAP_NET_RAW. Try: sudo " << argv[0] << " " << iface << "\n";
        return 1;
    }

    std::cout << "[bit_io] ecx_config_init (scanning bus)..." << std::endl;
    if (ecx_config_init(&context) <= 0) {
        std::cerr << "ecx_config_init failed: no EtherCAT slaves found on '" << iface << "'\n";
        ecx_close(&context);
        return 1;
    }

    const int n = context.slavecount;
    std::cout << "[bit_io] found " << n << " slave(s):" << std::endl;
    for (int i = 1; i <= n; ++i) {
        std::cout << "  [" << i << "] '" << context.slavelist[i].name << "'" << std::endl;
    }
    if (n < 1) {
        ecx_close(&context);
        return 1;
    }

    // Map process data into iomap, then configure DC (harmless if unused).
    ecx_config_map_group(&context, iomap, 0);
    ecx_configdc(&context);

    // --- SAFE_OP ---
    std::cout << "[bit_io] transitioning to SAFE_OP..." << std::endl;
    context.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, 0);
    ecx_statecheck(&context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);

    // --- OPERATIONAL ---
    std::cout << "[bit_io] transitioning to OPERATIONAL..." << std::endl;
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0);

    bool op_ready = false;
    for (int attempt = 0; attempt < 200 && !op_ready; ++attempt) {
        ecx_send_processdata(&context);
        ecx_receive_processdata(&context, EC_TIMEOUTRET);
        ecx_readstate(&context);

        for (int i = 1; i <= n; ++i) {
            if (context.slavelist[i].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
                context.slavelist[i].state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                ecx_writestate(&context, i);
            } else if (context.slavelist[i].state == EC_STATE_SAFE_OP) {
                context.slavelist[i].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context, i);
            }
        }

        ecx_statecheck(&context, 0, EC_STATE_OPERATIONAL, 50000); // 50 ms
        op_ready = (context.slavelist[0].state == EC_STATE_OPERATIONAL);
    }

    if (!op_ready) {
        std::cerr << "[bit_io] failed to reach OPERATIONAL:\n";
        for (int i = 1; i <= n; ++i) {
            const auto& s = context.slavelist[i];
            std::cerr << "  [" << i << "] '" << s.name << "' state=0x" << std::hex
                      << (int)s.state << " al=0x" << (int)s.ALstatuscode << std::dec
                      << " (" << ec_ALstatuscode2string(s.ALstatuscode) << ")\n";
        }
        ecx_close(&context);
        return 1;
    }
    std::cout << "[bit_io] OPERATIONAL." << std::endl;

    // Use the single slave on the bus.
    const int sid = 1;
    const auto& slave = context.slavelist[sid];

    // SOEM: Ibytes==0 when Ibits<8, so trust Ibits for bit count.
    const std::size_t in_bits   = slave.Ibits;
    const std::size_t out_bits  = slave.Obits;
    const std::size_t in_start  = slave.Istartbit;
    const std::size_t in_bytes  = bit_device::byteSpan(in_start, in_bits);

    std::cout << "[bit_io] slave '" << slave.name << "': "
              << "Ibits=" << in_bits << " (Istartbit=" << in_start << ", Ibytes=" << slave.Ibytes << ") "
              << "Obits=" << out_bits << " (Ostartbit=" << (int)slave.Ostartbit << ", Obytes=" << slave.Obytes << ")\n";

    if (slave.inputs == nullptr || in_bits == 0) {
        std::cerr << "[bit_io] slave has no input bits to read.\n";
        ecx_close(&context);
        return 1;
    }

    if (slave.outputs == nullptr || out_bits == 0) {
        std::cerr << "[bit_io] WARNING: slave has no output bits (outputs="
                  << (void*)slave.outputs << ", Obits=" << out_bits
                  << "). Writes will have no effect.\n";
    }

    const std::size_t out_bytes =
        bit_device::byteSpan(slave.Ostartbit, out_bits);

    // Display exactly the first 8 input bit channels (DI switches 1..8).
    constexpr std::size_t kDisplayChannels = 8;

    std::cout << "[bit_io] reading inputs (Ctrl-C to stop). Showing "
              << kDisplayChannels << " channels; channel 0 is the rightmost bit.\n";

    namespace bd = bit_device;

    // EtherCAT output watchdog (~100 ms typical) clears outputs unless process
    // data is exchanged frequently. Exchange fast; print at a human rate.
    constexpr int kCyclePeriodMs = 2;     // ~500 Hz on the wire
    constexpr int kPrintEveryN   = 100;   // print ~5x/sec

    int cycle = 0;
    while (g_run) {
        // Drive output channels in pattern 1010 1010 (ch7..ch0).
        // Channel 0 is the rightmost bit, so odd channels are high.
        for (std::size_t ch = 0; ch < kDisplayChannels; ++ch) {
            bd::writeBit(slave.outputs, slave.Ostartbit, ch, (ch % 2) == 1);
        }

        // Exchange process data on the wire.
        ecx_send_processdata(&context);
        const int wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);

        if (cycle % kPrintEveryN == 0) {
            const std::string in_hex  = bd::formatHex(slave.inputs, in_bytes);
            const std::string out_hex = bd::formatHex(slave.outputs, out_bytes);
            const std::string bits    = bd::formatBits(slave.inputs, in_start, kDisplayChannels);

            std::cout << "[" << cycle << "] wkc=" << wkc
                      << " out=[" << out_hex << "]"
                      << " in=[" << in_hex << "] bits=" << bits << "  |";
            for (std::size_t ch = 0; ch < kDisplayChannels; ++ch) {
                std::cout << " ch" << ch << "=" << (bd::readBit(slave.inputs, in_start, ch) ? 1 : 0);
            }
            std::cout << std::endl;
        }

        ++cycle;
        std::this_thread::sleep_for(std::chrono::milliseconds(kCyclePeriodMs));
    }

    std::cout << "\n[bit_io] stopping, returning to INIT..." << std::endl;
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0);
    ecx_close(&context);
    return 0;
}
