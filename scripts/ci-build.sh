#!/bin/sh
# SPDX-License-Identifier: MIT
# Build all images of one board on a Linux host, for CI.  The Makefile does
# the same on macOS, where Buildroot runs in a container.  Here Buildroot
# runs directly.
#
#   BOARD=esp32s31 scripts/ci-build.sh    (esp32s31, coreboard1 or korvo1)
#
# Needs: ESP-IDF at $IDF_PATH (the commit in bootloader/esp-idf.version and
# the patch commit of scripts/patch-esp-idf.sh), with its tools installed,
# and the Buildroot host packages.
# Output: build/BOARD/ with the flash images, and for korvo1 and coreboard1
# build/BOARD/buildroot/sdcard.img.
set -eu

cd "$(dirname "$0")/.."
: "${BOARD:=esp32s31}"
: "${IDF_PATH:=$HOME/esp/esp-idf}"
: "${BR_OUTPUT:=$PWD/build/br-output-$BOARD}"
: "${BR_DL_DIR:=$PWD/build/br-dl}"
export IDF_PATH BR2_DL_DIR="$BR_DL_DIR"

want=$(cat bootloader/esp-idf.version)
have=$(git -C "$IDF_PATH" rev-parse HEAD~1)
if [ "$want" != "$have" ]; then
	echo "ESP-IDF is on $have, but bootloader/esp-idf.version wants $want" >&2
	exit 1
fi

scripts/mkdefconfigs.sh --check
make bootloader BOARD="$BOARD"
make opensbi BOARD="$BOARD"

# The same steps as "make rootfs", without the container.
br="make -C external/buildroot O=$BR_OUTPUT BR2_EXTERNAL=$PWD/br2-external"
$br "${BOARD}_defconfig"
$br

out=build/$BOARD
mkdir -p "$out/buildroot"
# The 12-byte manifest is at the end of the linux partition, after the Image.
size=$(awk -F'[ ,]+' '$1 == "linux" { print $5 }' "bootloader/partitions-$BOARD.csv")
test "$(wc -c < "$BR_OUTPUT/images/Image")" -le $((size - 12))
cp "$BR_OUTPUT/images/Image" "$BR_OUTPUT/images/linux.size" "$out/"
for f in rootfs.ext2 sdcard.img; do
	if [ -f "$BR_OUTPUT/images/$f" ]; then
		cp "$BR_OUTPUT/images/$f" "$out/buildroot/"
	fi
done

# The root in the flash, on a board with a rootfs partition.
size=$(awk -F'[ ,]+' '$1 == "rootfs" { print $5 }' "bootloader/partitions-$BOARD.csv")
if [ -n "$size" ]; then
	test "$(wc -c < "$BR_OUTPUT/images/rootfs.squashfs")" -le $((size))
	cp "$BR_OUTPUT/images/rootfs.squashfs" "$out/"
fi
