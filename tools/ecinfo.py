#!/usr/bin/env python3
"""Read-only EtherCAT bus scan. Reads ESC registers only; writes nothing."""
import socket, struct, sys, time

ETH_P_ALL, ECAT_TYPE = 3, 0x88A4
APRD, BRD = 0x01, 0x07

class Bus:
    def __init__(self, iface):
        self.s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.s.bind((iface, 0))
        self.s.settimeout(0.5)
        self.src = self.s.getsockname()[4]
        self.idx = 0

    def cmd(self, cmd, adp, ado, dlen):
        self.idx = (self.idx + 1) & 0xFF
        dg = struct.pack('<BBhHHH', cmd, self.idx, adp, ado, dlen, 0) + b'\x00'*dlen + b'\x00\x00'
        pay = struct.pack('<H', (len(dg) & 0x7FF) | 0x1000) + dg
        pay += b'\x00' * max(0, 46 - len(pay))
        self.s.send(b'\xff'*6 + self.src + struct.pack('!H', ECAT_TYPE) + pay)
        end = time.time() + 0.5
        while time.time() < end:
            try: f = self.s.recv(2048)
            except socket.timeout: return None, 0
            if len(f) < 20 or struct.unpack('!H', f[12:14])[0] != ECAT_TYPE: continue
            if f[6:12] == self.src: continue          # our own outgoing copy
            if f[17] != self.idx: continue            # stale datagram index
            data = f[26:26+dlen]
            wkc = struct.unpack('<H', f[26+dlen:28+dlen])[0]
            return data, wkc
        return None, 0

    def u8(self, i, r):
        d, w = self.cmd(APRD, -i, r, 1)
        return (d[0] if d else None), w
    def u16(self, i, r):
        d, w = self.cmd(APRD, -i, r, 2)
        return (struct.unpack('<H', d)[0] if d else None), w
    def u32(self, i, r):
        d, w = self.cmd(APRD, -i, r, 4)
        return (struct.unpack('<I', d)[0] if d else None), w

AL = {1:'INIT', 2:'PREOP', 3:'BOOT', 4:'SAFEOP', 8:'OP'}
ALCODE = {0x0000:'no error', 0x0011:'invalid requested state change',
          0x0012:'unknown requested state', 0x0016:'invalid mailbox config (PREOP)',
          0x001B:'SM watchdog', 0x001D:'invalid output config',
          0x001E:'invalid input config', 0x0024:'invalid input mapping',
          0x002D:'sync error', 0x0030:'DC invalid sync config',
          0x0035:'DC sync0 cycle time', 0x004D:'sync error'}

def main():
    iface = sys.argv[1]
    b = Bus(iface)
    print(f"interface : {iface}  ({b.src.hex(':')})")
    d, wkc = b.cmd(BRD, 0, 0x0000, 1)
    if d is None:
        print("no answer to broadcast read -- no slave on this segment")
        return 1
    print(f"slaves    : {wkc}\n")
    for i in range(wkc):
        typ, _   = b.u8(i, 0x0000)
        rev, _   = b.u8(i, 0x0001)
        sm, _    = b.u8(i, 0x0005)
        fmmu, _  = b.u8(i, 0x0004)
        addr, _  = b.u16(i, 0x0010)
        dl, _    = b.u16(i, 0x0110)
        als, _   = b.u16(i, 0x0130)
        alc, _   = b.u16(i, 0x0134)
        st = als & 0x0F if als is not None else None
        err = bool(als & 0x10) if als is not None else False
        print(f"--- slave {i} (auto-inc {-i & 0xFFFF:#06x}) ---")
        print(f"  ESC type/rev : {typ:#04x} / {rev:#04x}    FMMUs {fmmu}  SyncManagers {sm}")
        print(f"  station addr : {addr:#06x}")
        ports = ' '.join(f"p{p}={'link' if dl >> (4+p) & 1 else 'open'}" for p in range(4)) if dl is not None else '?'
        print(f"  DL status    : {dl:#06x}   {ports}")
        print(f"  AL status    : {als:#06x} -> {AL.get(st,'?')}{'  +ERROR' if err else ''}")
        print(f"  AL status码  : {alc:#06x} ({ALCODE.get(alc, 'see ETG.1000 table')})")
    return 0

sys.exit(main())
