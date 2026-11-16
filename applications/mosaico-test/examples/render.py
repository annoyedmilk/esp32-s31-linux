#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render the documented examples with the native LVGL executable."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

WIDTH = 480
HEIGHT = 480
RGB_STRIDE = WIDTH * 3
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (
        struct.pack("!I", len(data)) + kind + data
        + struct.pack("!I", zlib.crc32(kind + data))
    )


def ppm_to_png(ppm: Path) -> bytes:
    # display_snapshot emits this fixed 480x480 RGB P6 format.
    header = f"P6\n{WIDTH} {HEIGHT}\n255\n".encode("ascii")
    source = ppm.read_bytes()
    if (not source.startswith(header)
            or len(source) != len(header) + HEIGHT * RGB_STRIDE):
        raise ValueError("Unexpected example image format")
    rgb = source[len(header):]
    rows = b"".join(
        b"\0" + rgb[y * RGB_STRIDE:(y + 1) * RGB_STRIDE]
        for y in range(HEIGHT)
    )
    return (
        PNG_SIGNATURE
        + png_chunk(b"IHDR", struct.pack("!2I5B", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(rows, 9))
        + png_chunk(b"IEND", b"")
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "renderer", type=Path, help="native mosaico-test-examples executable"
    )
    parser.add_argument(
        "--output-dir", type=Path, default=Path(__file__).resolve().parent,
        help="image directory (defaults to the examples directory)",
    )
    args = parser.parse_args()
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        for page in ("home", "imu", "compass"):
            image = Path(directory) / f"{page}.ppm"
            subprocess.run(
                [str(args.renderer.resolve()), page, str(image)], check=True
            )
            (output / f"{page}.png").write_bytes(ppm_to_png(image))
            print(f"Rendered {page}.png")


if __name__ == "__main__":
    main()
