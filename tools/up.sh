#!/bin/bash
# Bring the EtherCAT NIC up and wait for 100BASE-TX negotiation.
IFACE="${1:-enp133s0f1}"
ip link set "$IFACE" up
for i in $(seq 1 15); do
    if [ "$(cat /sys/class/net/$IFACE/carrier 2>/dev/null)" = "1" ]; then
        echo "carrier up after ${i}s"; break
    fi
    sleep 1
done
ip -br link show "$IFACE"
ethtool "$IFACE" | grep -E "Speed|Duplex|Link detected"
