#!/bin/sh
# SPDX-License-Identifier: MIT
# Show the 802.11 frames of the Wi-Fi channel with tcpdump, from a monitor
# interface next to wlan0.  The channel is the one of the station or the
# access point, and scans wait until the end.  Ctrl-C ends it; the monitor
# interface goes away.
#
#   wifi-monitor.sh [COUNT]

MON=mon0

[ $# -le 1 ] || { echo "usage: $0 [COUNT]" >&2; exit 1; }
phy=$(cat /sys/class/net/wlan0/phy80211/name) || exit 1
iw phy "$phy" interface add $MON type monitor || exit 1
trap 'iw dev $MON del' EXIT
trap 'exit 130' INT TERM
ip link set $MON up
tcpdump -i $MON -e -nn ${1:+-c "$1"}
