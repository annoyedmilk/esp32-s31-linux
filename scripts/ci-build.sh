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
# post-build.sh writes it into /etc/os-release; the release workflow sets it.
ESP32S31_VERSION=$(make -s version)
export ESP32S31_VERSION

scripts/mkdefconfigs.sh --check
make bootloader BOARD="$BOARD"
make opensbi BOARD="$BOARD"

# The same steps as "make rootfs", without the container.
br="make -C external/buildroot O=$BR_OUTPUT BR2_EXTERNAL=$PWD/br2-external"
$br "${BOARD}_defconfig"
$br

scripts/copy-images.sh "$BR_OUTPUT/images" "build/$BOARD" \
	"bootloader/partitions-$BOARD.csv"
