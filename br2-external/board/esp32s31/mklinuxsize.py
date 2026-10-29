#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the size manifest of a kernel Image: magic, size and CRC32.

The loader checks the Image with it before the copy to PSRAM.  The file is
the last flash sector of the linux partition: erased bytes, then the
manifest in the last 12 bytes.  esptool writes whole sectors, so a full
sector needs no erase of bytes that the file does not give.
"""

import struct
import sys
import zlib

SECTOR = 4096

image, manifest = sys.argv[1], sys.argv[2]
with open(image, "rb") as f:
    data = f.read()
with open(manifest, "wb") as f:
    manifest_data = struct.pack("<III", 0x455A4953, len(data), zlib.crc32(data))
    f.write(b"\xff" * (SECTOR - len(manifest_data)) + manifest_data)
print(f"{manifest}: {len(data)} bytes, crc32 {zlib.crc32(data):#010x}")
