#!/bin/bash
# Controlled A/B of an EtherCAT link: same cable, same slave, EEE off vs EEE on.
# Read-only as far as the bus is concerned -- only BRD register reads are sent.
#
#   sudo ./eclinktest.sh enp132s0 [count] [period_ms]
IFACE="${1:-enp132s0}"
COUNT="${2:-3000}"
PERIOD="${3:-1}"
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "############################################################"
echo "# EtherCAT link test on $IFACE"
echo "############################################################"

echo
echo "===== bringing the port up ====="
ip link set "$IFACE" up
for i in $(seq 1 15); do
    [ "$(cat /sys/class/net/$IFACE/carrier 2>/dev/null)" = "1" ] && break
    sleep 1
done
if [ "$(cat /sys/class/net/$IFACE/carrier 2>/dev/null)" != "1" ]; then
    echo "!! no carrier after 15 s -- cable unplugged, or the link cannot negotiate"
    echo "   (that alone is the answer: the PHY never got a stable link)"
    exit 1
fi
echo "carrier up after ${i}s"
ethtool "$IFACE" 2>/dev/null | grep -E "Speed|Duplex|Auto-negotiation:|Link detected"
echo "carrier_changes so far: $(cat /sys/class/net/$IFACE/carrier_changes 2>/dev/null)"

echo
echo "===== driver / PHY ====="
ethtool -i "$IFACE" 2>/dev/null | grep -E "driver|firmware|bus-info"
echo "--- EEE:"
ethtool --show-eee "$IFACE" 2>&1 | head -3

for MODE in off on; do
    echo
    echo "############################################################"
    echo "# run with EEE = $MODE"
    echo "############################################################"
    if ethtool --set-eee "$IFACE" eee "$MODE" 2>/dev/null; then
        echo "EEE set to $MODE"
    else
        echo "EEE not settable on this NIC (skipping the distinction)"
    fi
    sleep 2
    # An EEE change can renegotiate the link; wait for it to settle again.
    for i in $(seq 1 10); do
        [ "$(cat /sys/class/net/$IFACE/carrier)" = "1" ] && break
        sleep 1
    done
    "$DIR/ecstress.py" "$IFACE" --count "$COUNT" --period-ms "$PERIOD"
    [ "$(ethtool --show-eee "$IFACE" 2>/dev/null | grep -c .)" -eq 0 ] && break
done

echo
echo "===== link flap summary ====="
echo "carrier_changes total : $(cat /sys/class/net/$IFACE/carrier_changes 2>/dev/null)"
echo "--- recent kernel link events:"
journalctl -k --since "-10 min" --no-pager 2>/dev/null \
    | grep -iE "$IFACE.*(Link|link)" | tail -10
