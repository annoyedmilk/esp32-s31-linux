#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

# The version (make version) and the board of the image in os-release; ID
# stays buildroot for fastfetch.  An image without a version is an error.
version=${ESP32S31_VERSION:?post-build.sh: ESP32S31_VERSION is empty}
board=$(sed -n 's,^BR2_DEFCONFIG=".*/\(.*\)_defconfig"$,\1,p' "$BR2_CONFIG")
sed -i -e '/^PRETTY_NAME=/d' -e '/^IMAGE_ID=/d' -e '/^IMAGE_VERSION=/d' \
	"$1/usr/lib/os-release"
printf 'PRETTY_NAME="ESP32-S31 Linux %s"\nIMAGE_ID=esp32s31-linux-%s\nIMAGE_VERSION=%s\n' \
	"$version" "${board:?post-build.sh: no BR2_DEFCONFIG}" "$version" >>"$1/usr/lib/os-release"

# Log in on the serial console automatically.  SSH takes a key only.
grep -qF 'ttyS0::respawn:/sbin/getty -L -n -l /sbin/serial-autologin ttyS0 115200' "$1/etc/inittab" ||
	{ echo "post-build.sh: no autologin getty on ttyS0 in /etc/inittab" >&2; exit 1; }
test -x "$1/sbin/serial-autologin"

# BusyBox installs a start script for its httpd.  Here httpd is a tool.
rm -f "$1/etc/init.d/S90httpd"

# ROOT BIN LIBEXEC START: move BIN to LIBEXEC; in its place, a wrapper that
# runs START first.
wrap() {
	mkdir -p "$1/$3"
	mv "$1/$2" "$1/$3/${2##*/}"
	printf '#!/bin/sh\n%s && exec %s "$@"\n' "$4" "/$3/${2##*/}" >"$1/$2"
	chmod 755 "$1/$2"
}

# dbus-daemon and bluetoothd hold memory while idle: bluetooth-start starts
# them at the first bluetoothctl (or S40bluetooth), not the S prefix.
for s in S30dbus-daemon S40bluetoothd; do
	if [ -f "$1/etc/init.d/$s" ]; then
		mv "$1/etc/init.d/$s" "$1/etc/init.d/${s#S??}"
	fi
done
if ! grep -q bluetooth-start "$1/usr/bin/bluetoothctl"; then
	wrap "$1" usr/bin/bluetoothctl usr/libexec/bluetooth /usr/sbin/bluetooth-start
fi

# ot-daemon holds memory while idle: thread-start starts it at the first
# ot-ctl, as bluetooth-start does for bluetoothd.
for d in usr/sbin usr/bin; do
	if [ -x "$1/$d/ot-ctl" ] && ! grep -q thread-start "$1/$d/ot-ctl"; then
		wrap "$1" "$d/ot-ctl" usr/libexec/openthread /usr/sbin/thread-start
	fi
done

# Buildroot copies an overlay without removing old files, so the files of
# local-overlay/ stay in the root until make rootfs-clean: mark such a build
# for package-release.sh, next to the images.
if [ -n "$(find "$BR2_EXTERNAL_ESP32S31_PATH/../local-overlay" -mindepth 1 ! -name .empty)" ]; then
	: >"$BINARIES_DIR/local-overlay-used"
fi

# Buildroot copies the overlays with go=rX; local-overlay/ can add a key here.
chmod 600 "$1/etc/wpa_supplicant.conf"
if [ -e "$1/etc/default/mqtt" ]; then
	chmod 600 "$1/etc/default/mqtt"
fi

# The boot scripts stay on ash; root logs in with bash where it exists.
if [ -x "$1/bin/bash" ]; then
	sed -i '/^root:/s,[^:]*$,/bin/bash,' "$1/etc/passwd"
fi

# The flash root has no room for luac; lua compiles each script itself.
if [ -x "$1/sbin/overlay-init" ]; then
	rm -f "$1/usr/bin/luac"
fi

# A login on each VT in the arguments.  getty, not a shell: Ctrl-C needs a
# controlling terminal, and cttyhack opens the serial port instead.
inittab=$1/etc/inittab
shift
for vt in "$@"; do
	line="$vt::respawn:/sbin/getty -n -l /sbin/serial-autologin $vt 38400 linux"
	grep -qF "$line" "$inittab" || printf '%s\n' "$line" >>"$inittab"
done
