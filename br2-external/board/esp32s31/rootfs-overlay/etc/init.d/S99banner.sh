#!/bin/sh
# SPDX-License-Identifier: MIT
# make monitor waits for the banner: change it with the Makefile pattern.
# After the boot, only warnings reach the consoles (dmesg keeps all).

[ "$1" = start ] || exit 0

echo
echo "=== ESP32-S31 Linux / Buildroot ==="
echo 5 > /proc/sys/kernel/printk
