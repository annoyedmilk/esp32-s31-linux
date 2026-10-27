#!/bin/sh
# SPDX-License-Identifier: MIT
# A hub for Thread sensors.  Start the Thread network (a new one at the first
# run), print the dataset that the sensors need, then log each UDP datagram
# that comes to port 4242 with the time and the sender.  A sensor with the
# OpenThread CLI sends "udp open", then "udp send HUB 4242 23.5".
# Stop with Ctrl-C.  The network stays: thread-start stop ends it.
#
#   thread-hub.sh [LOG]     default: thread-hub.log on p1 of the disk

PORT=4242
. /etc/default/disk
LOG=${1:-/root/thread-hub.log}
if [ $# -eq 0 ] && [ -n "${DISK_MNT:-}" ] && mountpoint -q "$DISK_MNT"; then
	LOG=$DISK_MNT/thread-hub.log
fi

thread-start >/dev/null || exit 1
if ! ot-ctl dataset active >/dev/null 2>&1; then
	ot-ctl dataset init new >/dev/null && ot-ctl dataset commit active >/dev/null
fi
ot-ctl ifconfig up >/dev/null && ot-ctl thread start >/dev/null
# ot-ctl ends its lines with CR LF.
echo "Dataset for the sensors (ot-ctl: dataset set active HEX):"
ot-ctl dataset active -x | head -1 | tr -d '\r'
echo "Hub: $(ot-ctl ipaddr mleid | head -1 | tr -d '\r') port $PORT, log $LOG"
exec micropython /usr/share/examples/thread-hub.py $PORT "$LOG"
