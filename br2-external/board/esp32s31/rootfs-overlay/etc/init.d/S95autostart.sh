#!/bin/sh
# SPDX-License-Identifier: MIT
# Run autostart.sh at each boot: from p1 of the disk, which stays after
# "make sdroot" and which a Mac or PC can write, else from /root, which the
# overlay of the module alone keeps.

. /etc/default/disk
SCRIPT=${DISK_MNT:+$DISK_MNT/autostart.sh}
[ -f "$SCRIPT" ] || SCRIPT=/root/autostart.sh

[ "$1" = start ] && [ -f "$SCRIPT" ] || exit 0

echo "autostart: $SCRIPT, output in /tmp/autostart.log"
sh "$SCRIPT" </dev/null >/tmp/autostart.log 2>&1 &
