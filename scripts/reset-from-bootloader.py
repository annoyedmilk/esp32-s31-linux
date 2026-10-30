#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Leave ESP32-S31 ROM download mode without depending on reset wiring."""

import argparse
import sys

import serial
from esptool import FatalError
from esptool.targets.esp32s31 import ESP32S31ROM

LP_SYSTEM_CTRL = 0x20700008
FORCE_DOWNLOAD_BOOT = 1 << 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument(
        "--baud", type=int, default=115200,
        help="current ROM/stub baud rate (use 921600 after flashing at that rate)",
    )
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    try:
        # The CLI always starts at 115200. A resident flasher stub keeps its
        # last baud rate, so connect directly at the requested initial rate.
        with ESP32S31ROM(args.port, args.baud) as esp:
            esp.connect(mode="no-reset", attempts=3, warnings=False)
            value = esp.read_reg(LP_SYSTEM_CTRL)
            esp.write_reg(LP_SYSTEM_CTRL, value & ~FORCE_DOWNLOAD_BOOT)
            if esp.read_reg(LP_SYSTEM_CTRL) & FORCE_DOWNLOAD_BOOT:
                raise FatalError("forced-download bit did not clear")
            esp.watchdog_reset()
    except (FatalError, serial.SerialException) as error:
        print(f"reset failed: {error}", file=sys.stderr)
        return 1
    print("Download mode cleared; boot output is on the debug UART at 115200 baud.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
