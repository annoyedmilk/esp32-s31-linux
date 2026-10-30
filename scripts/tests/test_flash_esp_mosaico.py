# SPDX-License-Identifier: MIT
"""Keep the debug UART and unrelated USB devices out of the flashing path."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, Mock, patch

SCRIPT = Path(__file__).resolve().parents[1] / "flash-esp-mosaico.py"
SPEC = importlib.util.spec_from_file_location("flash_esp_mosaico", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.image = self.root / "test image.bin"
        self.image.write_bytes(b"firmware")
        self.args_file = self.root / "flash_args"
        self.args_file.write_text(f'0x210000 "{self.image}"' + chr(10))
        self.usb = SimpleNamespace(device="/dev/ttyACM2", vid=0x303A, pid=0x0020)
        self.rom = MagicMock()
        self.rom.__enter__.return_value = self.rom
        self.rom.uses_usb_otg.return_value = True
        self.stub = Mock()
        self.stub.read_reg.side_effect = [0x1234567C, 0x12345678]
        self.factory = Mock(return_value=self.rom)
        self.factory.ESPRESSIF_VID = 0x303A
        self.factory.IMAGE_CHIP_ID = 0x0020
        self.factory.FLASH_SECTOR_SIZE = 4096
        self.run_stub = Mock(return_value=self.stub)
        self.attach_flash = Mock()
        self.write_flash = Mock()

    def invoke(self, port=None, devices=None, images=None):
        argv = [str(SCRIPT), "--port", port or self.usb.device]
        argv += images if images is not None else [f"@{self.args_file}"]
        with (
            patch.object(sys, "argv", argv),
            patch.object(MODULE.list_ports, "comports", return_value=(
                devices if devices is not None else [self.usb]
            )),
            patch.object(MODULE, "ESP32S31ROM", self.factory),
            patch.object(MODULE, "run_stub", self.run_stub),
            patch.object(MODULE, "attach_flash", self.attach_flash),
            patch.object(MODULE, "write_flash", self.write_flash),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            return MODULE.main()

    def test_refuses_other_ports_before_opening_them(self):
        # CH343, Linux g_serial, Serial/JTAG and a keyboard may all be ttyACM.
        for identity in ((0x1A86, 0x55D3), (0x0525, 0xA4A7),
                         (0x303A, 0x1001), (0x4F57, 0x4110), (None, None)):
            with self.subTest(identity=identity):
                device = SimpleNamespace(
                    device="/dev/ttyACM1", vid=identity[0], pid=identity[1]
                )
                with self.assertRaises(SystemExit) as error:
                    self.invoke(device.device, [device])
                self.assertEqual(error.exception.code, 2)
                self.factory.assert_not_called()

    def test_flashes_and_resets_on_one_connection(self):
        device = self.root / "ttyACM2"
        device.touch()
        alias = self.root / "rom-usb"
        alias.symlink_to(device)
        self.usb.device = str(device)
        order = Mock()
        order.attach_mock(self.write_flash, "write_flash")
        order.attach_mock(self.stub, "stub")
        self.assertEqual(self.invoke(str(alias)), 0)
        self.factory.assert_called_once_with(str(alias), 115200)
        self.rom.connect.assert_called_once_with(
            mode="no-reset", attempts=3, warnings=False
        )
        self.run_stub.assert_called_once_with(self.rom)
        self.attach_flash.assert_called_once_with(self.stub)
        self.write_flash.assert_called_once_with(
            self.stub, [(0x210000, b"firmware")]
        )
        names = [call[0] for call in order.mock_calls]
        self.assertLess(names.index("write_flash"), names.index("stub.write_reg"))
        self.stub.write_reg.assert_called_once_with(
            MODULE.LP_SYSTEM_CTRL, 0x12345678
        )
        self.stub.watchdog_reset.assert_called_once_with()
        self.rom.__exit__.assert_called_once()

    def test_rejects_overlapping_erase_sectors_before_opening_port(self):
        images = ["0x210000", str(self.image), "0x210008", str(self.image)]
        with self.assertRaises(SystemExit) as error:
            self.invoke(images=images)
        self.assertEqual(error.exception.code, 2)
        self.factory.assert_not_called()

    def test_failed_flash_does_not_clear_download_or_reset(self):
        self.write_flash.side_effect = MODULE.FatalError("verification failed")
        self.assertEqual(self.invoke(), 1)
        self.stub.write_reg.assert_not_called()
        self.stub.watchdog_reset.assert_not_called()
        self.rom.__exit__.assert_called_once()

    def test_failed_flag_verification_does_not_reset(self):
        self.stub.read_reg.side_effect = [0x1234567C, 0x1234567C]
        self.assertEqual(self.invoke(), 1)
        self.stub.watchdog_reset.assert_not_called()

    def test_wrong_transport_does_not_write_flash(self):
        self.rom.uses_usb_otg.return_value = False
        self.assertEqual(self.invoke(), 1)
        self.run_stub.assert_not_called()
        self.write_flash.assert_not_called()

    def test_invalid_images_fail_before_opening_port(self):
        for images in (["0x210000"], ["bad", str(self.image)],
                       ["-1", str(self.image)], ["0", str(self.root / "missing")]):
            with self.subTest(images=images):
                with self.assertRaises(SystemExit) as error:
                    self.invoke(images=images)
                self.assertEqual(error.exception.code, 2)
                self.factory.assert_not_called()


if __name__ == "__main__":
    unittest.main()
