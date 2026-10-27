#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

cat > "${BINARIES_DIR}/sd-readme.txt" <<'TXT'
ESP32-S31 Linux disk: the microSD card of the Korvo-1, or the USB stick of
the Function-CoreBoard-1.

The board mounts this FAT32 partition on /mnt/sd (Korvo-1) or /mnt/usb
(Function-CoreBoard-1).  Use it for your files.  The second partition is
the ext4 root, the third is swap.  macOS cannot read them.
TXT

"${BOARD_DIR}/mklinuxsize.py" "${BINARIES_DIR}/Image" "${BINARIES_DIR}/linux.size"

# Only a board with a disk has an ext4 root and gets a disk image.
if [ -f "${BINARIES_DIR}/rootfs.ext2" ]; then
	support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
fi
