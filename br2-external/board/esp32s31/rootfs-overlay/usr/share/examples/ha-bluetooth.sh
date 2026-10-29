#!/bin/sh
# SPDX-License-Identifier: MIT
# ha-bluetooth.sh HA_ADDRESS...: make the board a Bluetooth proxy for Home
# Assistant at HA_ADDRESS (IPv4 or IPv6), from now on and after each boot.
# Home Assistant finds it with mDNS (ESPHome), or add the ESPHome
# integration with this board and port 6053.  While the proxy runs,
# bluetoothctl does not work.
# ha-bluetooth.sh off: no proxy.

CONF=/etc/default/ble-ha

case "$1" in
'')
	echo "usage: ha-bluetooth.sh HA_ADDRESS... | off" >&2
	exit 2
	;;
off)
	allow=""
	;;
*)
	allow="$*"
	;;
esac
sed -i "s/^BT_PROXY_ALLOW=.*/BT_PROXY_ALLOW=\"$allow\"/" $CONF
# The next boot must find the setting, also after a power cut.
sync
/etc/init.d/S45ble-ha restart
