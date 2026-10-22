#!/bin/sh
# SPDX-License-Identifier: MIT
# make monitor waits for this line.  If you change the text, also change
# --success-pattern in the Makefile.  login and SSH print /etc/motd.
# The consoles show the full kernel log of the boot.  After it, they show
# only warnings and errors, so that kernel messages do not write over the
# shell and tmux.  dmesg keeps all messages.

[ "$1" = start ] || exit 0

echo
echo "=== ESP32-S31 Linux / Buildroot ==="
echo 5 > /proc/sys/kernel/printk
