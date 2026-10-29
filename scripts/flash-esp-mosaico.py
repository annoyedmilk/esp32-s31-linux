#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Flash ESP-Mosaico through ROM USB while keeping the debug UART open."""

import argparse
from pathlib import Path
import shlex
import sys

from esptool import FatalError
from esptool.cmds import attach_flash, run_stub, write_flash
from esptool.targets.esp32s31 import ESP32S31ROM
from serial.tools import list_ports

LP_SYSTEM_CTRL = 0x20700008
FORCE_DOWNLOAD_BOOT = 1 << 2


class FlashArgumentParser(argparse.ArgumentParser):
    def convert_arg_line_to_args(self, line):
        return shlex.split(line, comments=True)


def validate_usb_port(name: str) -> None:
    """Refuse the debug UART and Linux gadgets without opening either one."""
    # Both a USB UART bridge and the Linux gadget can appear as ttyACM ports.
    # Check the USB identity before opening anything or sending reset signals.
    path = Path(name).resolve()
    port = next(
        (p for p in list_ports.comports() if Path(p.device).resolve() == path), None
    )
    if port is None or (port.vid, port.pid) != (
        ESP32S31ROM.ESPRESSIF_VID, ESP32S31ROM.IMAGE_CHIP_ID
    ):
        raise ValueError(
            "--port must identify the ESP32-S31 ROM USB downloader; "
            "connect the native Type-C port and run 'reboot bootloader' "
            "from the CH343 debug console first"
        )


def load_images(arguments: list[str]) -> list[tuple[int, bytes]]:
    """Load all images and validate their erase ranges before connecting."""
    if len(arguments) % 2:
        raise ValueError("images must be offset/file pairs")
    # Read inputs before connecting so a missing file leaves ROM untouched.
    images = []
    for offset, filename in zip(arguments[::2], arguments[1::2]):
        try:
            address = int(offset, 0)
            if address < 0:
                raise ValueError("offset must not be negative")
            images.append((address, Path(filename).read_bytes()))
        except (ValueError, OSError) as error:
            raise ValueError(f"{offset} {filename}: {error}") from error

    # NOR erases whole sectors. Reject overlaps before either image is lost.
    end = 0
    sector = ESP32S31ROM.FLASH_SECTOR_SIZE
    for address, data in sorted(images):
        if not data:
            raise ValueError(f"empty image at {address:#x}")
        start = address & ~(sector - 1)
        if start < end:
            raise ValueError(f"flash sectors overlap at {address:#x}")
        end = (address + len(data) + sector - 1) & ~(sector - 1)
    return images


def flash_and_reset(port: str, baud: int, images: list[tuple[int, bytes]]) -> None:
    with ESP32S31ROM(port, 115200) as rom:
        rom.connect(mode="no-reset", attempts=3, warnings=False)
        if not rom.uses_usb_otg():
            raise FatalError("the connected downloader is not native USB OTG")
        print("USB mode: USB-OTG")
        esp = run_stub(rom)
        if baud != 115200:
            esp.change_baud(baud)
        attach_flash(esp)
        write_flash(esp, images)

        # Keep the verified flash and reset on the same USB connection;
        # reconnecting to the active stub can fail as USB re-enumerates.
        value = esp.read_reg(LP_SYSTEM_CTRL)
        esp.write_reg(LP_SYSTEM_CTRL, value & ~FORCE_DOWNLOAD_BOOT)
        if esp.read_reg(LP_SYSTEM_CTRL) & FORCE_DOWNLOAD_BOOT:
            raise FatalError("forced-download bit did not clear")
        esp.watchdog_reset()


def main() -> int:
    parser = FlashArgumentParser(description=__doc__, fromfile_prefix_chars="@")
    parser.add_argument("--port", required=True, help="ESP32-S31 ROM USB CDC port")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("images", nargs="+", help="offset/file pairs or @flash_args")
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")

    try:
        validate_usb_port(args.port)
        images = load_images(args.images)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    try:
        flash_and_reset(args.port, args.baud, images)
    except (FatalError, OSError) as error:
        print(f"flash failed: {error}", file=sys.stderr)
        return 1
    print("Flash verified; boot output is on the debug UART at 115200 baud.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
