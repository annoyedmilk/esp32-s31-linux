#!/bin/sh
# SPDX-License-Identifier: MIT
# Commit the data partition at shutdown.  "umount -a -r" only detaches
# /overlay: the overlay root keeps its own mount of it, so UBIFS stays
# writable and needs a recovery at the next boot.  rcK runs this last.

case "$1" in
stop)
	mountpoint -q /overlay && mount -o remount,ro /overlay
	;;
esac

exit 0
