// EtherCAT slave structure dump — diagnose SII/EEPROM vs intended PDO sizes.
//
// Intent for the hand: Outputs (RxPDO, SM2) = 150 bytes, Inputs (TxPDO, SM3) = 180 bytes.
// If the device's SII EEPROM is mis-flashed, SOEM will report them swapped
// (Obytes=180 / Ibytes=150). This tool prints, for every slave, the full picture
// from THREE independent sources so you can see exactly where they disagree:
//
//   1. Mapped result      : slave.Obytes/Ibytes + FMMU directions (what SOEM will copy)
//   2. Live ESC registers : SM0..3 physical length + control byte (read over the wire)
//   3. SII EEPROM records  : the SM and PDO tables actually flashed on the chip
//   4. CoE objects (SDO)  : 0x1C12/0x1C13 assignment + 0x1601/0x1A00 entry counts
//
// Build: produced as `ecat_dump` by CMakeLists.txt.
// Run:   sudo ./ecat_dump <interface>      (defaults to enp132s0)

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

extern "C" {
#include <soem/soem.h>
}

namespace {

ecx_contextt g_ctx{};
uint8_t g_iomap[8192]{};

const char* smTypeName(uint8_t t)
{
    switch (t) {
        case 0: return "unused";
        case 1: return "MbxOut";
        case 2: return "MbxIn";
        case 3: return "Outputs(RxPDO)";
        case 4: return "Inputs(TxPDO)";
        default: return "?";
    }
}

const char* fmmuFuncName(uint8_t f)
{
    switch (f) {
        case 0: return "unused";
        case 1: return "outputs";
        case 2: return "inputs";
        case 3: return "SM-status";
        default: return "?";
    }
}

// Read a 16-bit ESC register over the wire (FPRD by configured address).
uint16_t readReg16(uint16_t configadr, uint16_t reg)
{
    return ecx_FPRDw(&g_ctx.port, configadr, reg, EC_TIMEOUTRET);
}

void dumpMappedResult(int i)
{
    const auto& s = g_ctx.slavelist[i];
    std::cout << "  --- (1) Mapped result (what SOEM will copy on the wire) ---\n";
    std::cout << "    Obytes=" << s.Obytes << " (Obits=" << s.Obits << ")   <-- master OUTPUT / RxPDO / command\n";
    std::cout << "    Ibytes=" << s.Ibytes << " (Ibits=" << s.Ibits << ")   <-- master INPUT  / TxPDO / state\n";
    std::cout << "    outputs ptr=" << (void*)s.outputs << "  inputs ptr=" << (void*)s.inputs << "\n";
    std::cout << "    FMMU0func=" << fmmuFuncName(s.FMMU0func)
              << " FMMU1func=" << fmmuFuncName(s.FMMU1func)
              << " FMMU2func=" << fmmuFuncName(s.FMMU2func)
              << " FMMU3func=" << fmmuFuncName(s.FMMU3func) << "\n";

    for (int f = 0; f < EC_MAXFMMU; ++f) {
        const auto& m = s.FMMU[f];
        if (m.LogLength == 0) continue;
        std::cout << "    FMMU[" << f << "] LogLength=" << m.LogLength
                  << " PhysStart=0x" << std::hex << m.PhysStart << std::dec
                  << " LogStart=0x" << std::hex << m.LogStart << std::dec
                  << " FMMUtype=" << (int)m.FMMUtype
                  << " (" << fmmuFuncName(m.FMMUtype) << ")\n";
    }
}

void dumpEscRegisters(int i)
{
    const auto& s = g_ctx.slavelist[i];
    const uint16_t adr = s.configadr;
    std::cout << "  --- (2) Live ESC SM registers (read from chip over the wire) ---\n";
    // SM0..SM3: each block is 8 bytes. Offset +0 = phys start addr (16b),
    // +2 = length (16b), +4 = control byte (8b).
    const uint16_t smBase[4] = {ECT_REG_SM0, ECT_REG_SM1, ECT_REG_SM2, ECT_REG_SM3};
    for (int sm = 0; sm < 4; ++sm) {
        const uint16_t start = readReg16(adr, smBase[sm] + 0);
        const uint16_t len   = readReg16(adr, smBase[sm] + 2);
        const uint16_t ctrl  = readReg16(adr, smBase[sm] + 4); // low byte = control
        std::cout << "    SM" << sm
                  << "  physStart=0x" << std::hex << std::setw(4) << std::setfill('0') << start
                  << "  length=" << std::dec << len
                  << "  ctrl=0x" << std::hex << std::setw(2) << std::setfill('0') << (ctrl & 0xFF)
                  << std::dec << std::setfill(' ');
        if (sm == 2) std::cout << "   <-- SM2 should be Outputs/command = 150";
        if (sm == 3) std::cout << "   <-- SM3 should be Inputs/state    = 180";
        std::cout << "\n";
    }
    std::cout << "    SOEM SMtype[]: ";
    for (int sm = 0; sm < EC_MAXSM; ++sm) {
        if (s.SMtype[sm] == 0 && sm > 3) break;
        std::cout << "SM" << sm << "=" << smTypeName(s.SMtype[sm]) << " ";
    }
    std::cout << "\n";
    std::cout << "    SOEM cached SM[] lengths: ";
    for (int sm = 0; sm < 4; ++sm) {
        std::cout << "SM" << sm << "(len=" << s.SM[sm].SMlength
                  << ",start=0x" << std::hex << s.SM[sm].StartAddr << std::dec << ") ";
    }
    std::cout << "\n";
}

void dumpSiiEeprom(int i)
{
    std::cout << "  --- (3) SII EEPROM records (what is actually flashed on the chip) ---\n";

    // Force reading the EEPROM (not the cached PDI copy).
    ecx_eeprom2master(&g_ctx, i);

    // SM section from EEPROM.
    ec_eepromSMt esm{};
    uint16_t eectl = ecx_siiSM(&g_ctx, i, &esm);
    if (eectl > 0) {
        std::cout << "    SII SM count=" << (int)esm.nSM << "\n";
        // First entry is in esm; iterate the rest with siiSMnext.
        for (uint16_t n = 0; n < esm.nSM; ++n) {
            ec_eepromSMt cur = esm;
            if (n > 0) {
                if (ecx_siiSMnext(&g_ctx, i, &cur, n) == 0) break;
            }
            std::cout << "    SII SM" << n
                      << "  PhStart=0x" << std::hex << std::setw(4) << std::setfill('0') << cur.PhStart
                      << "  Plength=" << std::dec << cur.Plength
                      << "  Creg=0x" << std::hex << std::setw(2) << std::setfill('0') << (int)cur.Creg
                      << "  Activate=0x" << std::setw(2) << (int)cur.Activate
                      << std::dec << std::setfill(' ');
            if (n == 2) std::cout << "   <-- intended 150";
            if (n == 3) std::cout << "   <-- intended 180";
            std::cout << "\n";
        }
    } else {
        std::cout << "    (no SII SM section found)\n";
    }

    // PDO tables from EEPROM: t=0 -> output(RxPDO), t=1 -> input(TxPDO).
    ec_eepromPDOt pdo{};
    uint32_t outbits = ecx_siiPDO(&g_ctx, i, &pdo, 0);
    std::cout << "    SII RxPDO(output) total bits=" << outbits
              << " (" << outbits / 8 << " bytes), nPDO=" << pdo.nPDO << "\n";
    for (int sm = 0; sm < EC_MAXSM; ++sm) {
        if (pdo.SMbitsize[sm])
            std::cout << "        SMbitsize[" << sm << "]=" << pdo.SMbitsize[sm]
                      << " (" << pdo.SMbitsize[sm] / 8 << " B)\n";
    }

    std::memset(&pdo, 0, sizeof(pdo));
    uint32_t inbits = ecx_siiPDO(&g_ctx, i, &pdo, 1);
    std::cout << "    SII TxPDO(input)  total bits=" << inbits
              << " (" << inbits / 8 << " bytes), nPDO=" << pdo.nPDO << "\n";
    for (int sm = 0; sm < EC_MAXSM; ++sm) {
        if (pdo.SMbitsize[sm])
            std::cout << "        SMbitsize[" << sm << "]=" << pdo.SMbitsize[sm]
                      << " (" << pdo.SMbitsize[sm] / 8 << " B)\n";
    }

    // Hand back EEPROM control to the PDI (slave application).
    ecx_eeprom2pdi(&g_ctx, i);
}

void readSdoU8(int i, uint16_t idx, uint8_t sub, const char* label)
{
    uint8_t val = 0;
    int sz = sizeof(val);
    int wkc = ecx_SDOread(&g_ctx, i, idx, sub, FALSE, &sz, &val, EC_TIMEOUTRXM);
    std::cout << "    " << label << " (0x" << std::hex << idx << ":" << std::dec << (int)sub
              << ") = " << (int)val << (wkc > 0 ? "" : "  [read FAILED]") << "\n";
}

void readSdoU16(int i, uint16_t idx, uint8_t sub, const char* label)
{
    uint16_t val = 0;
    int sz = sizeof(val);
    int wkc = ecx_SDOread(&g_ctx, i, idx, sub, FALSE, &sz, &val, EC_TIMEOUTRXM);
    std::cout << "    " << label << " (0x" << std::hex << idx << ":" << std::dec << (int)sub
              << ") = 0x" << std::hex << val << std::dec
              << (wkc > 0 ? "" : "  [read FAILED]") << "\n";
}

void dumpCoe(int i)
{
    std::cout << "  --- (4) CoE objects via SDO (firmware's declared mapping) ---\n";
    readSdoU8 (i, 0x1C00, 0, "SM type count");
    readSdoU8 (i, 0x1C00, 1, "  SM1 type");
    readSdoU8 (i, 0x1C00, 2, "  SM2 type");
    readSdoU8 (i, 0x1C00, 3, "  SM3 type");
    readSdoU8 (i, 0x1C12, 0, "RxPDO assign count (SM2)");
    readSdoU16(i, 0x1C12, 1, "  RxPDO assign[1]");
    readSdoU8 (i, 0x1C13, 0, "TxPDO assign count (SM3)");
    readSdoU16(i, 0x1C13, 1, "  TxPDO assign[1]");
    readSdoU8 (i, 0x1601, 0, "RxPDO 0x1601 entry count");
    readSdoU8 (i, 0x1A00, 0, "TxPDO 0x1A00 entry count");
    // For R-hand the maps may be 0x1602 / 0x1A01:
    readSdoU8 (i, 0x1602, 0, "RxPDO 0x1602 entry count");
    readSdoU8 (i, 0x1A01, 0, "TxPDO 0x1A01 entry count");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string iface = (argc > 1) ? argv[1] : "enp132s0";

    std::cout << "[dump] ecx_init on '" << iface << "'...\n";
    if (!ecx_init(&g_ctx, iface.c_str())) {
        std::cerr << "ecx_init failed (need sudo / CAP_NET_RAW)\n";
        return 1;
    }

    if (ecx_config_init(&g_ctx) <= 0) {
        std::cerr << "ecx_config_init: no slaves found\n";
        ecx_close(&g_ctx);
        return 1;
    }

    const int n = g_ctx.slavecount;
    std::cout << "[dump] found " << n << " slave(s)\n";

    // Map so Obytes/Ibytes/FMMU are populated. (We are in SAFE_OP-ish PRE_OP here.)
    ecx_config_map_group(&g_ctx, g_iomap, 0);
    ecx_configdc(&g_ctx);

    for (int i = 1; i <= n; ++i) {
        const auto& s = g_ctx.slavelist[i];
        std::cout << "\n================ Slave [" << i << "] '" << s.name << "' ================\n";
        std::cout << "  configadr=0x" << std::hex << s.configadr << std::dec
                  << "  eep_id=0x" << std::hex << s.eep_id
                  << "  eep_rev=0x" << s.eep_rev << std::dec << "\n";

        dumpMappedResult(i);
        dumpEscRegisters(i);
        dumpSiiEeprom(i);
        dumpCoe(i);

        // Verdict against the stated intent.
        std::cout << "  --- VERDICT (intent: Outputs=150, Inputs=180) ---\n";
        const bool ok = (s.Obytes == 150 && s.Ibytes == 180);
        if (ok) {
            std::cout << "    OK: device matches intent.\n";
        } else {
            std::cout << "    MISMATCH: Obytes=" << s.Obytes << " (want 150), Ibytes="
                      << s.Ibytes << " (want 180).\n";
            if (s.Obytes == 180 && s.Ibytes == 150)
                std::cout << "    => SM2/SM3 are SWAPPED in the SII EEPROM. Re-flash SII.\n";
        }
    }

    std::cout << "\n[dump] done. Returning to INIT.\n";
    g_ctx.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&g_ctx, 0);
    ecx_close(&g_ctx);
    return 0;
}
