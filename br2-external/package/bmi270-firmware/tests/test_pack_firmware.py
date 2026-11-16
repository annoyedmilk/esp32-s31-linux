# SPDX-License-Identifier: MIT
"""Check BMI270 extraction and reject unverified initialization images."""

import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "pack-firmware.py"
SPEC = importlib.util.spec_from_file_location("pack_firmware", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def config_table(entries):
    return f"const uint8_t bmi270_config_file[] = {{{entries}}};"


class FirmwareTests(unittest.TestCase):
    def test_extracts_comments_and_validated_bytes(self):
        image = bytes((0x12, 0xAB, 0x00))
        with (
            patch.object(MODULE, "IMAGE_SIZE", len(image)),
            patch.object(MODULE, "IMAGE_SHA256", hashlib.sha256(image).hexdigest()),
        ):
            source = config_table("/* header */ 0x12, 0xab, // note\n 0x00,")
            self.assertEqual(MODULE.extract_image(source), image)

    def test_rejects_missing_or_commented_table(self):
        for source in ("no table", "/* " + config_table("0x00") + " */"):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, "not found"):
                MODULE.extract_image(source)

    def test_rejects_malformed_byte_lists(self):
        for entries in ("", "0x100", "0x00 0x01", "0x00,,0x01", "0x00 + 1", "VALUE"):
            with self.subTest(entries=entries), self.assertRaisesRegex(ValueError, "Unexpected"):
                MODULE.extract_image(config_table(entries))

    def test_rejects_wrong_image_size(self):
        with self.assertRaisesRegex(ValueError, "8192 bytes"):
            MODULE.extract_image(config_table("0x00"))

    def test_rejects_wrong_digest(self):
        with self.assertRaisesRegex(ValueError, "pinned version"):
            MODULE.extract_image(config_table("0x00," * 8192))

    def test_cli_reports_missing_source_without_traceback(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(directory / "missing.c"),
                 str(directory / "output.fw")],
                capture_output=True, timeout=5,
            )
            self.assertEqual(result.returncode, 1)
            self.assertNotIn(b"Traceback", result.stderr)
            self.assertFalse((directory / "output.fw").exists())


if __name__ == "__main__":
    unittest.main()
