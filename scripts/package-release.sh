#!/bin/sh
# SPDX-License-Identifier: MIT
# Package the build output for a release: a flash bundle, a card image and
# the checksums.  Run after "make build" or scripts/ci-build.sh.
#
#   scripts/package-release.sh v0.1.0
#
# Output in build/release/.
set -eu

cd "$(dirname "$0")/.."
version=${1:?usage: $0 VERSION}
name=esp32s31-linux-$version
out=build/release
stage=$out/$name-flash
esptool=${ESPTOOL:-python3 -m esptool}

rm -rf "$out"
mkdir -p "$stage"

# The flash regions, from the same list as "make flash".
args=$(make -s flash-args)
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

cat > "$stage/FLASH.txt" <<TXT
ESP32-S31 Linux $version for the ESP32-S31-Korvo-1

Install esptool 5.4 or later:  pip install "esptool>=5.4"
Connect the UART Type-C port (CP2102N). Then flash one of the two ways:

  esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin
  esptool --chip esp32s31 -p PORT -b 921600 write-flash @flash_args

Write the card image to a microSD card (it erases the card), for example
with balenaEtcher, or with dd on Linux or macOS:

  xz -dc $name-sdcard.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync

Console: the same UART port, 115200 8N1. Root password: korvo-bringup.
TXT

(cd "$out" && zip -qr "$name-flash.zip" "$name-flash")
xz -T0 -9 -c build/buildroot/sdcard.img > "$out/$name-sdcard.img.xz"
rm -rf "$stage"
(cd "$out" && sha256sum "$name-flash.zip" "$name-sdcard.img.xz" > SHA256SUMS 2>/dev/null ||
	shasum -a 256 "$name-flash.zip" "$name-sdcard.img.xz" > SHA256SUMS)
ls -l "$out"
