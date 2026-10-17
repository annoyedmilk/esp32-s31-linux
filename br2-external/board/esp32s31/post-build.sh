#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

# Log in on the serial console automatically.  SSH still needs the root password.
grep -qF 'ttyS0::respawn:/sbin/getty -L -n -l /sbin/serial-autologin ttyS0 115200' "$1/etc/inittab"
test -x "$1/sbin/serial-autologin"

# A login on each VT in the arguments: on the panel, for a USB keyboard.
# Use getty, not a shell: a shell needs the VT as its controlling terminal, or
# Ctrl-C does nothing.  cttyhack cannot do this, because it opens the last
# console in console/active, which is the serial port.
target=$1
shift
for vt in "$@"; do
	line="$vt::respawn:/sbin/getty -n -l /sbin/serial-autologin $vt 38400 linux"
	grep -qF "$line" "$target/etc/inittab" || printf '%s\n' "$line" >> "$target/etc/inittab"
done
set -- "$target"

# BusyBox installs a start script for its httpd.  Here httpd is a tool.  The
# kernel has no module support, so S11modules has nothing to load.
rm -f "$1/etc/init.d/S90httpd" "$1/etc/init.d/S11modules"

mkdir -p "$1/mnt/sd"

# The boot scripts run as /bin/sh, BusyBox ash, which needs much less memory
# than bash.  The interactive shell of root is bash, where the image has it.
if [ -x "$1/bin/bash" ]; then
	sed -i '/^root:/s,[^:]*$,/bin/bash,' "$1/etc/passwd"
fi
