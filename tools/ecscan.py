#!/usr/bin/env python3
# Read-only EtherCAT probe: BRD (broadcast read) of register 0x0000, 1 byte.
# Counts how many slaves answer (working counter). Changes nothing on the bus.
import socket, struct, sys, time

iface = sys.argv[1]
ETH_P_ALL = 3
ECAT_TYPE = 0x88A4

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
s.bind((iface, 0))
s.settimeout(1.0)
src = s.getsockname()[4]

# EtherCAT datagram: cmd=BRD(0x07), idx, ADP=0, ADO=0x0000, len=1, irq=0, data, wkc
dgram = struct.pack('<BBHHHH', 0x07, 0x01, 0x0000, 0x0000, 1, 0x0000) + b'\x00' + b'\x00\x00'
ecat_hdr = struct.pack('<H', (len(dgram) & 0x7FF) | (0x1 << 12))  # len | type=1
payload = ecat_hdr + dgram
if len(payload) < 46:
    payload += b'\x00' * (46 - len(payload))
frame = b'\xff\xff\xff\xff\xff\xff' + src + struct.pack('!H', ECAT_TYPE) + payload

print(f"iface={iface} src_mac={src.hex(':')}")
s.send(frame)

deadline = time.time() + 1.0
got = False
while time.time() < deadline:
    try:
        data = s.recv(2048)
    except socket.timeout:
        break
    if len(data) < 14 or struct.unpack('!H', data[12:14])[0] != ECAT_TYPE:
        continue
    if data[6:12] == src and data[0:6] == b'\xff'*6:
        continue  # our own outgoing copy
    wkc = struct.unpack('<H', data[14+2+10+1:14+2+10+3])[0]
    print(f"RESPONSE  len={len(data)}  dst={data[0:6].hex(':')}  src={data[6:12].hex(':')}  WKC={wkc}")
    print(f"  -> {wkc} slave(s) answered the broadcast read")
    got = True
    break

if not got:
    print("NO RESPONSE - no EtherCAT slave answered on this interface")
