#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract the standard BMI270 image from the pinned Bosch SensorAPI."""

import argparse
import hashlib
from pathlib import Path
import re

IMAGE_SIZE = 8192
IMAGE_SHA256 = "2d75e68e343a13ff99be98261dfbd99d9e8c6f267da9e3c5aeb883276ad178db"


def extract_image(source: str) -> bytes:
    """Validate the vendor array before accepting it as sensor firmware."""
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    table = re.search(
        r"\bconst\s+uint8_t\s+bmi270_config_file\s*\[\s*\]\s*=\s*\{(.*?)\};",
        source, re.S,
    )
    if table is None:
        raise ValueError("BMI270 initialization table not found")

    body = table[1]
    tokens = body.strip().removesuffix(",").split(",")
    if any(
        re.fullmatch(r"\s*0x[0-9a-fA-F]{2}\s*", token) is None
        for token in tokens
    ):
        raise ValueError("Unexpected BMI270 initialization data")
    image = bytes(int(token, 16) for token in tokens)
    if len(image) != IMAGE_SIZE:
        raise ValueError(f"BMI270 initialization image must contain {IMAGE_SIZE} bytes")
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError("BMI270 initialization image does not match the pinned version")
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="pinned Bosch bmi270.c source")
    parser.add_argument("output", type=Path, help="destination firmware file")
    args = parser.parse_args()
    try:
        image = extract_image(args.source.read_text(encoding="utf-8"))
        args.output.write_bytes(image)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
