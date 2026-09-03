#!/usr/bin/env python3
"""Read-only EtherCAT link stress test.

Sends N broadcast-read (BRD) datagrams at a fixed period -- the same shape of
traffic the cyclic loop produces -- and reports loss rate, round-trip time
distribution, and the NIC's own error counters over the same window.

Reads register 0x0000 only. Writes nothing to any slave.

    sudo ./ecstress.py enp132s0 --count 2000 --period-ms 1
"""
import argparse, os, socket, struct, sys, time

ETH_P_ALL, ECAT_TYPE, BRD = 3, 0x88A4, 0x07

STAT_FIELDS = [
    'rx_packets', 'tx_packets', 'rx_errors', 'tx_errors', 'rx_dropped',
    'tx_dropped', 'rx_crc_errors', 'rx_frame_errors', 'rx_fifo_errors',
    'rx_missed_errors', 'rx_length_errors', 'tx_carrier_errors',
    'tx_fifo_errors', 'collisions',
]


def read_stats(iface):
    out = {}
    base = f'/sys/class/net/{iface}/statistics'
    for f in STAT_FIELDS:
        try:
            with open(os.path.join(base, f)) as fh:
                out[f] = int(fh.read().strip())
        except OSError:
            pass
    for extra in ('carrier_changes', 'carrier_up_count', 'carrier_down_count'):
        try:
            with open(f'/sys/class/net/{iface}/{extra}') as fh:
                out[extra] = int(fh.read().strip())
        except OSError:
            pass
    return out


def pct(sorted_vals, p):
    if not sorted_vals:
        return float('nan')
    k = min(len(sorted_vals) - 1, int(round((p / 100.0) * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('iface')
    ap.add_argument('--count', type=int, default=2000)
    ap.add_argument('--period-ms', type=float, default=1.0)
    ap.add_argument('--timeout-ms', type=float, default=20.0)
    a = ap.parse_args()

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((a.iface, 0))
    s.settimeout(a.timeout_ms / 1000.0)
    src = s.getsockname()[4]

    print(f"interface : {a.iface} ({src.hex(':')})")
    print(f"plan      : {a.count} BRD frames, one every {a.period_ms} ms, "
          f"{a.timeout_ms} ms timeout\n")

    before = read_stats(a.iface)
    rtts, lost, wkc_bad, wkc_seen = [], 0, 0, set()
    idx = 0
    t_start = time.perf_counter()
    next_send = t_start

    for n in range(a.count):
        now = time.perf_counter()
        if next_send > now:
            time.sleep(next_send - now)
        next_send += a.period_ms / 1000.0

        idx = (idx + 1) & 0xFF
        dg = struct.pack('<BBhHHH', BRD, idx, 0, 0x0000, 1, 0) + b'\x00' + b'\x00\x00'
        pay = struct.pack('<H', (len(dg) & 0x7FF) | 0x1000) + dg
        pay += b'\x00' * max(0, 46 - len(pay))
        frame = b'\xff' * 6 + src + struct.pack('!H', ECAT_TYPE) + pay

        t0 = time.perf_counter()
        try:
            s.send(frame)
        except OSError as e:
            print(f"  send failed at frame {n}: {e}")
            lost += 1
            continue

        deadline = t0 + a.timeout_ms / 1000.0
        got = False
        while time.perf_counter() < deadline:
            try:
                f = s.recv(2048)
            except socket.timeout:
                break
            if len(f) < 30 or struct.unpack('!H', f[12:14])[0] != ECAT_TYPE:
                continue
            if f[6:12] == src:          # our own outgoing copy
                continue
            if f[17] != idx:            # a stale reply from an earlier frame
                continue
            wkc = struct.unpack('<H', f[27:29])[0]
            wkc_seen.add(wkc)
            if wkc < 1:
                wkc_bad += 1
            rtts.append((time.perf_counter() - t0) * 1e6)
            got = True
            break
        if not got:
            lost += 1

    elapsed = time.perf_counter() - t_start
    after = read_stats(a.iface)

    answered = len(rtts)
    print(f"--- results over {elapsed:.1f} s ---")
    print(f"  sent            : {a.count}")
    print(f"  answered        : {answered}")
    print(f"  lost / timeout  : {lost}   ({100.0 * lost / a.count:.3f} %)")
    print(f"  bad WKC (<1)    : {wkc_bad}")
    print(f"  WKC values seen : {sorted(wkc_seen) if wkc_seen else 'none'}")

    if rtts:
        r = sorted(rtts)
        print(f"\n--- round-trip time (us) ---")
        print(f"  min {r[0]:8.1f}   avg {sum(r)/len(r):8.1f}   max {r[-1]:8.1f}")
        print(f"  p50 {pct(r,50):8.1f}   p99 {pct(r,99):8.1f}   "
              f"p99.9 {pct(r,99.9):8.1f}")
        print(f"  jitter (max-min) : {r[-1] - r[0]:.1f} us")

    print(f"\n--- NIC counter deltas ---")
    any_err = False
    for k in sorted(set(before) | set(after)):
        d = after.get(k, 0) - before.get(k, 0)
        if d:
            print(f"  {k:22s} +{d}")
            if 'error' in k or k == 'collisions' or 'carrier_' in k:
                any_err = True
    if not any_err:
        print("  (no error counters moved)")

    print(f"\n--- verdict ---")
    if lost == 0 and not any_err:
        print("  link looks clean over this window")
    else:
        if after.get('rx_crc_errors', 0) - before.get('rx_crc_errors', 0):
            print("  CRC errors -> PHYSICAL layer: cable, length, or interference")
        if after.get('carrier_changes', 0) - before.get('carrier_changes', 0):
            print("  carrier flapped -> link cannot stay negotiated")
        if lost and not any_err:
            print("  frames lost with NO counter movement -> frames never made it")
            print("  onto the wire, or replies were dropped before the counters;")
            print("  suspect driver/scheduling rather than the cable")
    return 0


sys.exit(main())
