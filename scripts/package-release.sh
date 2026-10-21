#!/bin/sh
# SPDX-License-Identifier: MIT
# Package the build output of one board for a release: a flash bundle and,
# for a board with a disk, a disk image.  Run after "make build" or
# scripts/ci-build.sh for the same board.
#
#   scripts/package-release.sh v0.1.0 korvo1
#
# Output in build/release/.  Run it for each board, then make the checksums.
set -eu

cd "$(dirname "$0")/.."
# The images of a local build contain the files of local-overlay/.
if [ -n "$(ls -A local-overlay | grep -vx .empty)" ]; then
	echo "local-overlay/ has files, which can be keys: do not release this build" >&2
	exit 1
fi
version=${1:?usage: $0 VERSION BOARD}
board=${2:?usage: $0 VERSION BOARD}
name=esp32s31-linux-$version-$board
out=build/release
stage=$out/$name-flash
card=build/$board/buildroot/sdcard.img
esptool=${ESPTOOL:-python3 -m esptool}
esptool_version=$(cat bootloader/esptool.version)
have=$($esptool version 2>/dev/null | tail -1)
if [ "$have" != "$esptool_version" ]; then
	echo "esptool is $have, but bootloader/esptool.version wants $esptool_version" >&2
	exit 1
fi

rm -rf "$stage"
mkdir -p "$stage"

# The flash regions, from the same list as "make flash".
args=$(make -s flash-args BOARD="$board")
flash_args=
set -- $args
while [ $# -ge 2 ]; do
	test -f "$2" || { echo "missing $2: build first" >&2; exit 1; }
	cp "$2" "$stage/"
	flash_args="$flash_args $1 $(basename "$2")"
	shift 2
done
echo "$flash_args" | sed 's/^ //' > "$stage/flash_args"

# One image for offset 0, for the simplest flash command.
(cd "$stage" && $esptool --chip esp32s31 merge-bin -o flash.bin $(cat flash_args))

disk=sdcard
case $board in
korvo1)
	title="the ESP32-S31-Korvo-1 (with the ESP32-S31-WROOM-3-N16R16V)"
	usage="Write the card image to a microSD card (it erases the card), for example
with balenaEtcher, or with dd on Linux or macOS:

  xz -dc $name-$disk.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync" ;;
coreboard1)
	title="the ESP32-S31-Function-CoreBoard-1"
	disk=usb
	usage="Write the disk image to a USB stick (it erases the stick), for example
with balenaEtcher, or with dd on Linux or macOS:

  xz -dc $name-$disk.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync

The board mounts its root from the stick with this image (disk signature
0x45533331), so other USB disks can be connected when it starts." ;;
esp32s31)
	title="the ESP32-S31-WROOM-3-N16R16V module alone (UART only)"
	usage="The full system is in the flash.  The root is writable, and the files
stay after a reboot and after a new flash.bin.  A data partition without UBI
(a new board, or a board from v0.4.0) is erased and formatted at the first
boot.  To go back to the image, run flash_erase /dev/mtd1 0 0 and reboot." ;;
esac

cat > "$stage/FLASH.txt" <<TXT
ESP32-S31 Linux $version for $title

Install the tested esptool:  pip install "esptool==$esptool_version"
Connect the UART port of UART0. Then flash one of the two ways:

  esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin
  esptool --chip esp32s31 -p PORT -b 921600 write-flash @flash_args

$usage

Console: the same UART port, 115200 8N1, logged in as root. SSH takes root
only with a key: add it to /root/.ssh/authorized_keys from the console.
TXT

(cd "$out" && zip -qr "$name-flash.zip" "$name-flash")
rm -rf "$stage"
if [ -f "$card" ]; then
	xz -T0 -9 -c "$card" > "$out/$name-$disk.img.xz"
fi
# The checksums of all packages in build/release, also of other boards.
(cd "$out" && rm -f SHA256SUMS &&
	set -- $(ls) &&
	{ sha256sum "$@" 2>/dev/null || shasum -a 256 "$@"; } > SHA256SUMS)
ls -l "$out"
