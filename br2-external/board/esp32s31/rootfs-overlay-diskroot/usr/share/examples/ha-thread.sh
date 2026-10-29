#!/bin/sh
# SPDX-License-Identifier: MIT
# ha-thread.sh HA_ADDRESS...: give the Thread radio to Home Assistant at
# HA_ADDRESS (IPv4 or IPv6), from now on and after each boot.  In Home
# Assistant, the OpenThread Border Router app then needs a dummy device
# and network_device set to this board, port 6638.
# ha-thread.sh off: ot-daemon on the board is the Thread host again.

CONF=/etc/default/thread

set_conf()
{
	sed -i "s/^THREAD_MODE=.*/THREAD_MODE=$1/; s/^RELAY_ALLOW=.*/RELAY_ALLOW=\"$2\"/" $CONF
	# The next boot must find the mode, also after a power cut.
	sync
}

case "$1" in
'')
	echo "usage: ha-thread.sh HA_ADDRESS... | off" >&2
	exit 2
	;;
off)
	thread-start stop
	set_conf daemon ""
	echo "ha-thread: ot-daemon starts at the next ot-ctl"
	;;
*)
	thread-start stop
	set_conf relay "$*"
	if ! thread-start; then
		set_conf daemon ""
		exit 1
	fi
	ip=$(ip -4 addr show eth0 2>/dev/null; ip -4 addr show wlan0 2>/dev/null)
	ip=$(echo "$ip" | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
	echo "ha-thread: in Home Assistant, network_device: ${ip:-<board address>}:6638"
	;;
esac
