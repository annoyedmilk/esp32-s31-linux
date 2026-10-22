#!/bin/sh
# SPDX-License-Identifier: MIT
# Start a DHCP client on eth0 of the Function-CoreBoard-1.  udhcpc goes to
# the background at once and keeps trying, so the boot does not wait for a
# cable.  Without a PHY the link does not come up, and udhcpc would stay in
# the foreground.
# stmmac allocates its rings when the interface goes up.  Its default RX ring
# of 512 pages would take 2 MiB of the 16 MiB, so make the rings smaller first.

IFACE=eth0
PID=/var/run/udhcpc.$IFACE.pid

case "$1" in
start)
	[ -d /sys/class/net/$IFACE ] || exit 0
	ethtool -G $IFACE rx 64 tx 64
	ip link set $IFACE up || exit 0
	udhcpc -b -S -i $IFACE -p $PID
	;;
stop)
	[ -r $PID ] && kill "$(cat $PID)" 2>/dev/null
	rm -f $PID
	ip addr flush dev $IFACE 2>/dev/null
	ip link set $IFACE down 2>/dev/null
	;;
restart)
	"$0" stop
	sleep 1
	"$0" start
	;;
esac

exit 0
