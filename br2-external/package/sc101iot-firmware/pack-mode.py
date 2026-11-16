#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack the pinned vendor VGA mode into the SC101IOT driver's register format."""

import argparse
from pathlib import Path
import re
import struct

MODE_TABLE = "sc101iot_dvp_8bit_20Minput_640x480_yuv422_uyvy_15fps"
MAX_REGISTERS = 512
FIRMWARE_MAGIC = b"SC10"
FIRMWARE_VERSION = 1
# Little-endian magic, version, register count and reserved word.
HEADER = struct.Struct("<4sHHI")
REGISTER_ENTRY = re.compile(
    r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}"
)


def parse_registers(source: str) -> bytes:
    """Read only the pinned VGA table, rejecting unsupported C expressions."""
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    table = re.search(
        rf"\b{MODE_TABLE}\s*\[\s*\]\s*=\s*\{{(.*?)\}}\s*;", source, re.S
    )
    if table is None:
        raise ValueError(f"SC101IOT mode table not found: {MODE_TABLE}")

    # The vendor enum is not defined in the mode header. UYVY is value 2.
    body = re.sub(r"\bSC101IOT_YUV422_FMT_UYVY\b", "0x02", table[1])
    payload = bytearray()
    cursor = 0
    for entry in REGISTER_ENTRY.finditer(body):
        separator = body[cursor:entry.start()].strip()
        if separator != ("," if cursor else ""):
            raise ValueError("Unexpected data between SC101IOT register entries")
        address, value = (int(token, 16) for token in entry.groups())
        if address > 0xFF or value > 0xFF:
            raise ValueError("SC101IOT register addresses and values must fit in a byte")
        payload.extend((address, value))
        cursor = entry.end()

    if body[cursor:].strip() not in ("", ","):
        raise ValueError("Unexpected data at the end of the SC101IOT mode table")
    if not 1 <= len(payload) // 2 <= MAX_REGISTERS:
        raise ValueError(f"SC101IOT mode must contain 1 to {MAX_REGISTERS} registers")
    return bytes(payload)


def pack_mode(source: str) -> bytes:
    payload = parse_registers(source)
    register_count = len(payload) // 2
    header = HEADER.pack(FIRMWARE_MAGIC, FIRMWARE_VERSION, register_count, 0)
    return header + payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="pinned vendor VGA mode header")
    parser.add_argument("output", type=Path, help="destination firmware file")
    args = parser.parse_args()
    try:
        firmware = pack_mode(args.source.read_text(encoding="utf-8"))
        args.output.write_bytes(firmware)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
