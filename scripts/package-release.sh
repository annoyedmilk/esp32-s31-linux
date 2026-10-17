#!/bin/sh
# SPDX-License-Identifier: MIT
# Package the build output of one board for a release: a flash bundle and,
# for a board with a card, a card image.  Run after "make build" or
# scripts/ci-build.sh for the same board.
#
#   scripts/package-release.sh v0.1.0 korvo1
#
# Output in build/release/.  Run it for each board, then make the checksums.
set -eu

cd "$(dirname "$0")/.."
version=${1:?usage: $0 VERSION BOARD}
board=${2:?usage: $0 VERSION BOARD}
name=esp32s31-linux-$version-$board
out=build/release
stage=$out/$name-flash
card=build/$board/buildroot/sdcard.img
esptool=${ESPTOOL:-python3 -m esptool}

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

case $board in
korvo1)
	title="the ESP32-S31-Korvo-1"
	usage="Write the card image to a microSD card (it erases the card), for example
with balenaEtcher, or with dd on Linux or macOS:

  xz -dc $name-sdcard.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync" ;;
esp32s31)
	title="a bare ESP32-S31 module with 16 MiB PSRAM (UART only)"
	usage="The full system is in the flash.  To keep files such as the Wi-Fi
network after a reboot, run: persist save" ;;
esac

cat > "$stage/FLASH.txt" <<TXT
ESP32-S31 Linux $version for $title

Install esptool 5.4 or later:  pip install "esptool>=5.4"
Connect the UART port of UART0. Then flash one of the two ways:

  esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin
  esptool --chip esp32s31 -p PORT -b 921600 write-flash @flash_args

$usage

Console: the same UART port, 115200 8N1. Root password: korvo-bringup.
TXT

(cd "$out" && zip -qr "$name-flash.zip" "$name-flash")
rm -rf "$stage"
if [ -f "$card" ]; then
	xz -T0 -9 -c "$card" > "$out/$name-sdcard.img.xz"
fi
# The checksums of all packages in build/release, also of other boards.
(cd "$out" && rm -f SHA256SUMS &&
	set -- $(ls) &&
	{ sha256sum "$@" 2>/dev/null || shasum -a 256 "$@"; } > SHA256SUMS)
ls -l "$out"
