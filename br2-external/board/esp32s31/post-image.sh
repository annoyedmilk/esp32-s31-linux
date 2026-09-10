#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

cat > "${BINARIES_DIR}/sd-readme.txt" <<'TXT'
ESP32-S31 Linux SD card.

This FAT32 partition is yours; the board mounts it on /mnt/sd. The second
partition is the ext4 root and is not readable from macOS.

The card must stay MBR -- the kernel has no GPT support and would see no
partitions at all.
TXT

support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
