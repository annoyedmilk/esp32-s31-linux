#!/bin/sh
# SPDX-License-Identifier: MIT
# rcK runs this last. Flush the NAND upper layer before the reset.

case "$1" in
stop)
	mountpoint -q /overlay && mount -o remount,ro /overlay
	;;
esac

exit 0
