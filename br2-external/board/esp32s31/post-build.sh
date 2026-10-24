#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

# Log in on the serial console automatically.  SSH takes a key only.
grep -qF 'ttyS0::respawn:/sbin/getty -L -n -l /sbin/serial-autologin ttyS0 115200' "$1/etc/inittab"
test -x "$1/sbin/serial-autologin"

# A login on each VT in the arguments.  getty, not a shell: Ctrl-C needs a
# controlling terminal, and cttyhack opens the serial port instead.
target=$1
shift
for vt in "$@"; do
	line="$vt::respawn:/sbin/getty -n -l /sbin/serial-autologin $vt 38400 linux"
	grep -qF "$line" "$target/etc/inittab" || printf '%s\n' "$line" >> "$target/etc/inittab"
done
set -- "$target"

# BusyBox installs a start script for its httpd.  Here httpd is a tool.
rm -f "$1/etc/init.d/S90httpd"

# dbus-daemon and bluetoothd hold memory while idle: bluetooth-start starts
# them at the first bluetoothctl (or S40bluetooth), not the S prefix.
for s in S30dbus-daemon S40bluetoothd; do
	if [ -f "$1/etc/init.d/$s" ]; then
		mv "$1/etc/init.d/$s" "$1/etc/init.d/${s#S??}"
	fi
done
if ! grep -q bluetooth-start "$1/usr/bin/bluetoothctl"; then
	mv "$1/usr/bin/bluetoothctl" "$1/usr/libexec/bluetooth/bluetoothctl"
	printf '#!/bin/sh\n%s\n' \
		'/usr/sbin/bluetooth-start && exec /usr/libexec/bluetooth/bluetoothctl "$@"' \
		>"$1/usr/bin/bluetoothctl"
	chmod 755 "$1/usr/bin/bluetoothctl"
fi

# Buildroot copies the overlays with go=rX; local-overlay/ can add a key here.
chmod 600 "$1/etc/wpa_supplicant.conf"

# The boot scripts stay on ash; root logs in with bash where it exists.
if [ -x "$1/bin/bash" ]; then
	sed -i '/^root:/s,[^:]*$,/bin/bash,' "$1/etc/passwd"
fi

# The flash root has no room for luac; lua compiles each script itself.
if [ -x "$1/sbin/overlay-init" ]; then
	rm -f "$1/usr/bin/luac"
fi
