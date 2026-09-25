#!/bin/sh
# SPDX-License-Identifier: MIT
# Build all images on a Linux host, for CI.  The Makefile does the same on
# macOS, where Buildroot runs in a container.  Here Buildroot runs directly.
#
# Needs: ESP-IDF at $IDF_PATH (the commit in bootloader/esp-idf.version),
# with its tools installed, and the Buildroot host packages.
# Output: build/ with the flash images and build/buildroot/sdcard.img.
set -eu

cd "$(dirname "$0")/.."
: "${IDF_PATH:=$HOME/esp/esp-idf}"
: "${BR_OUTPUT:=$PWD/build/br-output}"
: "${BR_DL_DIR:=$PWD/build/br-dl}"
export IDF_PATH BR2_DL_DIR="$BR_DL_DIR"

want=$(cat bootloader/esp-idf.version)
have=$(git -C "$IDF_PATH" rev-parse HEAD)
if [ "$want" != "$have" ]; then
	echo "ESP-IDF is at $have, but bootloader/esp-idf.version wants $want" >&2
	exit 1
fi

jobs=$(nproc)
make kernel-patches
make bootloader
make opensbi JOBS="$jobs"

# The same steps as "make rootfs", without the container.
br="make -C external/buildroot O=$BR_OUTPUT BR2_EXTERNAL=$PWD/br2-external"
$br esp32s31_defconfig
$br

mkdir -p build/buildroot
cp "$BR_OUTPUT/images/Image" "$BR_OUTPUT/images/linux.size" build/
cp "$BR_OUTPUT/images/rootfs.ext2" "$BR_OUTPUT/images/sdcard.img" build/buildroot/
python3 scripts/mkinitramfs.py --target "$BR_OUTPUT/target" \
	--init br2-external/board/esp32s31/init \
	--output build/initramfs.cpio --size 0x200000
