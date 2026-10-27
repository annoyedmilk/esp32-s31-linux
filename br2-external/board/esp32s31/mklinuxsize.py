#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the size manifest of a kernel Image: magic, size and CRC32.

The loader checks the Image with it before the copy to PSRAM.  make flash
writes it at the end of the linux partition.
"""

import struct
import sys
import zlib

image, manifest = sys.argv[1], sys.argv[2]
with open(image, "rb") as f:
    data = f.read()
with open(manifest, "wb") as f:
    f.write(struct.pack("<III", 0x455A4953, len(data), zlib.crc32(data)))
print(f"{manifest}: {len(data)} bytes, crc32 {zlib.crc32(data):#010x}")
