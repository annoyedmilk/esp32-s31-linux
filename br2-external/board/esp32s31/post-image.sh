#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

cat > "${BINARIES_DIR}/sd-readme.txt" <<'TXT'
ESP32-S31 Linux SD card.

The board mounts this FAT32 partition on /mnt/sd.  Use it for your files.
The second partition is the ext4 root.  macOS cannot read it.
TXT

# The loader checks the Image with this manifest before it copies the Image
# to PSRAM.  make flash writes it at LINUX_SIZE_OFFSET.
python3 - "${BINARIES_DIR}" <<'PY'
import struct, sys, zlib, pathlib

images = pathlib.Path(sys.argv[1])
data = (images / "Image").read_bytes()
manifest = struct.pack("<III", 0x455A4953, len(data), zlib.crc32(data))
(images / "linux.size").write_bytes(manifest)
print("linux.size: %d bytes, crc32 %#010x" % (len(data), zlib.crc32(data)))
PY

support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
