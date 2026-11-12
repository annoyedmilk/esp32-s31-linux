#!/bin/sh
# SPDX-License-Identifier: MIT
# Start an access point with WPA2 on channel 6 and the address 192.168.4.1.
# udhcpd gives the clients 192.168.4.100 to 192.168.4.200.  Without --nat,
# the clients get no default route and no DNS: they reach only the board.
# With --nat IFACE, the board routes the clients to IFACE (for example eth0)
# with NAT, and gives them its own DNS servers.
# /etc/init.d/S40wifi restart goes back to the station.
#
#   ap.sh SSID PASSPHRASE [--nat IFACE]

IFACE=wlan0
NET=192.168.4
CONF=/tmp/ap.conf
DHCP_CONF=/tmp/udhcpd.$IFACE.conf

usage()
{
	echo "usage: $0 SSID PASSPHRASE [--nat IFACE]" >&2
	exit 1
}

[ $# -eq 2 ] || { [ $# -eq 4 ] && [ "$3" = --nat ]; } || usage
uplink=$4
[ -z "$uplink" ] || [ -d /sys/class/net/$uplink ] ||
	{ echo "$0: no interface $uplink" >&2; exit 1; }
[ "$uplink" != $IFACE ] || usage

umask 077
cat > $CONF <<CONF
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
wpa_supplicant -B -s -D nl80211 -i $IFACE -c $CONF -P /var/run/wpa_supplicant.$IFACE.pid
ip addr add $NET.1/24 dev $IFACE

# A second DHCP server on a network breaks it: serve only in AP mode.
for _ in $(seq 1 10); do
	iw dev $IFACE info | grep -q 'type AP' && break
	sleep 1
done
iw dev $IFACE info | grep -q 'type AP' ||
	{ echo "$0: $IFACE is not an access point, no DHCP server" >&2; exit 1; }

{
	echo "interface $IFACE"
	echo "start $NET.100"
	echo "end $NET.200"
	echo "lease_file /var/run/udhcpd.$IFACE.leases"
	echo "option subnet 255.255.255.0"
	echo "option lease 3600"
	if [ -n "$uplink" ]; then
		echo "option router $NET.1"
		echo "option dns $(awk '$1 == "nameserver" { print $2 }' /etc/resolv.conf | tr '\n' ' ')"
	fi
} > $DHCP_CONF
touch /var/run/udhcpd.$IFACE.leases
# This BusyBox writes no pidfile for udhcpd: S40wifi stop needs the pid.
udhcpd -f $DHCP_CONF </dev/null >/dev/null 2>&1 &
echo $! > /var/run/udhcpd.$IFACE.pid
nft add element inet filter ap_dhcp "{ $IFACE }"

if [ -n "$uplink" ]; then
	sysctl -q -w net.ipv4.ip_forward=1
	nft add table ip ap-nat
	nft "add chain ip ap-nat postrouting { type nat hook postrouting priority srcnat; }"
	nft add rule ip ap-nat postrouting ip saddr $NET.0/24 oifname $uplink masquerade
	nft add element inet filter ap_route "{ $IFACE . $uplink }"
	echo "ap: routing $NET.0/24 to $uplink"
fi
echo "ap: $1 on $IFACE, $NET.1, DHCP $NET.100-200"
