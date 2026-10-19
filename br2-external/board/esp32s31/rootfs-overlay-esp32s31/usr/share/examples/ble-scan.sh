#!/bin/sh
# SPDX-License-Identifier: MIT
# Scan for Bluetooth LE devices, then print the devices that BlueZ knows.
#
#   ble-scan.sh [SECONDS]

{ echo "scan le"; sleep "${1:-10}"; echo "scan off"; echo devices; sleep 1; echo quit; } |
	bluetoothctl | sed 's/\x1b\[[0-9;]*[mK]//g' | grep '^Device'
