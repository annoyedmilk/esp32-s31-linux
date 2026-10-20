#!/bin/sh
# SPDX-License-Identifier: MIT
# Print the touches on the LCD.  The GT1151 touch controller is on the I2C
# bus, and the goodix driver polls it.  Stop with Ctrl-C.

dev=$(awk '/^N: Name="Goodix/ { found = 1 }
	found && /^H: Handlers=/ {
		for (i = 2; i <= NF; i++)
			if ($i ~ /^event/)
				print "/dev/input/" $i
		exit
	}' /proc/bus/input/devices)
[ -n "$dev" ] || { echo "no touch controller" >&2; exit 1; }
exec evtest "$dev"
