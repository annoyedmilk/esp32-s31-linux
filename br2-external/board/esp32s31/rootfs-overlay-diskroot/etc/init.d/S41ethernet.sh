#!/bin/sh
# SPDX-License-Identifier: MIT
# Start a DHCP client on eth0: the GMAC of the Function-CoreBoard-1, or a
# USB Ethernet adapter of the Korvo-1, at boot or when mdev reports a new
# adapter (mdev.conf).  udhcpc -b goes to the background after its discover
# attempts and keeps trying.  Without a link, udhcpc would retry in the
# foreground and stop the boot: do not start it.  The default RX ring of
# stmmac (512 pages) would take 2 MiB of the 16 MiB, so make the rings
# smaller before the interface goes up; only the stmmac board has ethtool.

IFACE=eth0
PID=/var/run/udhcpc.$IFACE.pid

case "$1" in
start)
	[ -d /sys/class/net/$IFACE ] || exit 0
	# The boot and a hotplug event can both start it: one at a time, until
	# udhcpc has written its pidfile.  udhcpc must not keep the lock.
	exec 9>/var/run/udhcpc.$IFACE.lock
	flock 9
	[ -r $PID ] && kill -0 "$(cat $PID)" 2>/dev/null && exit 0
	command -v ethtool >/dev/null && ethtool -G $IFACE rx 64 tx 64
	ip link set $IFACE up || exit 0
	udhcpc -b -S -i $IFACE -p $PID 9>&-
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
hotplug)
	[ "$MDEV" = $IFACE ] || exit 0
	if [ "$ACTION" = add ]; then
		"$0" start
	else
		"$0" stop
	fi
	;;
esac

exit 0
