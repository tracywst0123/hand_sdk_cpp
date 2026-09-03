#!/bin/bash
# Make the EtherCAT NIC come up at boot, since NetworkManager no longer
# manages it. Review this file before running it -- it installs a systemd unit.
set -e
IFACE="${1:-enp133s0f1}"
UNIT=/etc/systemd/system/ethercat-nic.service

{
    echo "[Unit]"
    echo "Description=Bring up EtherCAT NIC ${IFACE}"
    echo "After=network-pre.target"
    echo "Wants=network-pre.target"
    echo
    echo "[Service]"
    echo "Type=oneshot"
    echo "RemainAfterExit=yes"
    echo "ExecStart=/usr/sbin/ip link set ${IFACE} up"
    echo
    echo "[Install]"
    echo "WantedBy=multi-user.target"
} > "$UNIT"

echo "--- installed $UNIT:"
cat "$UNIT"
systemctl daemon-reload
systemctl enable --now ethercat-nic.service
systemctl --no-pager status ethercat-nic.service | head -5
