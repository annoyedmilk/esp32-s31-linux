#!/bin/sh
# SPDX-License-Identifier: MIT
# Build all images of one board on a Linux host, for CI.  The Makefile does
# the same on macOS, where Buildroot runs in a container.  Here Buildroot
# runs directly.
#
#   BOARD=korvo1 scripts/ci-build.sh      (korvo1 or esp32s31)
#
# Needs: ESP-IDF at $IDF_PATH (the commit in bootloader/esp-idf.version and
# the patch commit of scripts/patch-esp-idf.sh), with its tools installed,
# and the Buildroot host packages.
# Output: build/BOARD/ with the flash images, and for korvo1
# build/korvo1/buildroot/sdcard.img.
set -eu

cd "$(dirname "$0")/.."
: "${BOARD:=korvo1}"
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

make kernel-patches
make bootloader BOARD="$BOARD"
make opensbi BOARD="$BOARD"

# The same steps as "make rootfs", without the container.
br="make -C external/buildroot O=$BR_OUTPUT BR2_EXTERNAL=$PWD/br2-external"
$br "${BOARD}_defconfig"
$br

out=build/$BOARD
mkdir -p "$out/buildroot"
cp "$BR_OUTPUT/images/Image" "$BR_OUTPUT/images/linux.size" "$out/"
for f in rootfs.ext2 sdcard.img; do
	if [ -f "$BR_OUTPUT/images/$f" ]; then
		cp "$BR_OUTPUT/images/$f" "$out/buildroot/"
	fi
done

# The same initramfs as the Makefile makes for the board.
case $BOARD in
korvo1)	src="--target $BR_OUTPUT/target --init br2-external/board/esp32s31/init" ;;
esp32s31)	src="--cpio $BR_OUTPUT/images/rootfs.cpio.gz" ;;
esac
python3 scripts/mkinitramfs.py $src --output "$out/initramfs.cpio" --size 0x3F0000
