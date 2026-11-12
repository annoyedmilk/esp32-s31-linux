#!/bin/sh
# SPDX-License-Identifier: MIT
# Copy the Buildroot images of a board to its build directory, after a check
# that each one fits its partition.  The Makefile (br-artifacts, in the
# container) and scripts/ci-build.sh both use it.
#
#   scripts/copy-images.sh IMAGES_DIR OUT_DIR PARTITION_CSV
set -eu

img=$1
out=$2
parts=$3

# Field 5 of a row of the partition table is the size.
part_size() { awk -F'[ ,]+' -v n="$1" '$1 == n { print $5 }' "$parts"; }

# FILE MAX TEXT: stop when FILE has more than MAX bytes.
fits() {
	test "$(wc -c < "$1")" -le $(($2)) ||
		{ echo "$(basename "$1") is larger than $3" >&2; exit 1; }
}

mkdir -p "$out/buildroot"
# linux.size is the last sector of the linux partition, after the Image.
size=$(part_size linux)
fits "$img/Image" "$size - 4096" "the $size partition without its last sector"
cp "$img/Image" "$img/linux.size" "$out/"

# The root in the flash, on a board with a rootfs partition.
size=$(part_size rootfs)
if [ -n "$size" ]; then
	fits "$img/rootfs.squashfs" "$size" "the $size partition"
	cp "$img/rootfs.squashfs" "$out/"
fi

for f in rootfs.ext2 sdcard.img; do
	if [ -f "$img/$f" ]; then
		cp "$img/$f" "$out/buildroot/"
	fi
done
