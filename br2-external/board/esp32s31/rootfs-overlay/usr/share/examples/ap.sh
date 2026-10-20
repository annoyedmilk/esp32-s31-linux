#!/bin/sh
# SPDX-License-Identifier: MIT
# Start an access point with WPA2 on channel 6 and the address 192.168.4.1.
# There is no DHCP server: give the client a static address, for example
# 192.168.4.2/24.  /etc/init.d/S40wifi restart goes back to the station.
#
#   ap.sh SSID PASSPHRASE

[ $# -eq 2 ] || { echo "usage: $0 SSID PASSPHRASE" >&2; exit 1; }
conf=/tmp/ap.conf
umask 077
cat > $conf <<CONF
ctrl_interface=/var/run/wpa_supplicant
network={
	ssid="$1"
	mode=2
	frequency=2437
	key_mgmt=WPA-PSK
	proto=RSN
	pairwise=CCMP
	group=CCMP
	psk="$2"
}
CONF
/etc/init.d/S40wifi stop
wpa_supplicant -B -s -D nl80211 -i wlan0 -c $conf -P /var/run/wpa_supplicant.wlan0.pid
ip addr add 192.168.4.1/24 dev wlan0
