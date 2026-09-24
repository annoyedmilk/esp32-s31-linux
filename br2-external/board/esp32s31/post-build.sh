#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

# Log in on the serial console automatically.  SSH still needs the root password.
grep -qF 'ttyS0::respawn:/sbin/getty -L -n -l /sbin/serial-autologin ttyS0 115200' "$1/etc/inittab"
test -x "$1/sbin/serial-autologin"

# A login on the panel, for a USB keyboard.  Use getty, not a shell: a shell
# needs tty1 as its controlling terminal, or Ctrl-C does nothing.  cttyhack
# cannot do this, because it opens the last console in console/active, which
# is the serial port.
tty1_line='tty1::respawn:/sbin/getty -n -l /sbin/serial-autologin tty1 38400 linux'
grep -qF "$tty1_line" "$1/etc/inittab" || printf '%s\n' "$tty1_line" >> "$1/etc/inittab"

mkdir -p "$1/mnt/sd"
