#!/bin/bash
# Stop NetworkManager from claiming the EtherCAT port.
set -e
CONF=/etc/NetworkManager/conf.d/99-ethercat-unmanaged.conf
cp -n "$CONF" "$CONF.bak" 2>/dev/null || true
cat > "$CONF" <<'CONFEOF'
[keyfile]
unmanaged-devices=interface-name:enp132s0;interface-name:enp133s0f1
CONFEOF
echo "--- $CONF now:"; cat "$CONF"
# Drop the auto-generated wired profile that keeps retrying DHCP.
nmcli -t -f NAME,UUID con show | grep '^Wired connection 3:' | cut -d: -f2 \
  | xargs -r -n1 nmcli con delete || true
systemctl reload NetworkManager
sleep 2
echo "--- device state:"; nmcli -t device status | grep -E 'enp13|enp132'

# NetworkManager has now let go of the port, which also means nothing holds it
# UP any more -- it drops to DOWN the moment it goes unmanaged. Bring it back
# and wait for 100BASE-TX autonegotiation before anyone tries to scan.
echo "--- bringing the port back up:"
"$(dirname "$0")/up.sh" enp133s0f1
