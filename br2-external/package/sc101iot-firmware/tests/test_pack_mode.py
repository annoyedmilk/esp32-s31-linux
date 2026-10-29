# SPDX-License-Identifier: MIT
"""Check the SC101IOT mode parser without redistributing vendor register data."""

import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "pack-mode.py"
SPEC = importlib.util.spec_from_file_location("pack_mode", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def mode_table(entries):
    return f"static const sc101iot_reginfo_t {MODULE.MODE_TABLE}[] = {{{entries}}};"


class ModeTests(unittest.TestCase):
    def test_packs_only_named_table_with_comments_and_format_enum(self):
        source = "static const int unrelated[] = {99};\n"
        source += mode_table("""
            /* register page */ {0xf0, 0x01},
            {0x10, SC101IOT_YUV422_FMT_UYVY}, // pixel order
            {0x20, 0xff}
        """)
        firmware = MODULE.pack_mode(source)
        self.assertEqual(firmware[:12], struct.pack("<4sHHI", b"SC10", 1, 3, 0))
        self.assertEqual(firmware[12:], bytes((0xF0, 1, 0x10, 2, 0x20, 0xFF)))

    def test_rejects_missing_or_unrelated_table(self):
        with self.assertRaisesRegex(ValueError, "mode table not found"):
            MODULE.pack_mode("static const int unrelated[] = {0};")

    def test_rejects_expressions_and_malformed_separators(self):
        invalid_entries = (
            "{0x00, OTHER_ENUM}",
            "{0x00, 0x01} {0x02, 0x03}",
            ",{0x00, 0x01}",
            "{0x00, 0x01},,",
            "{0x00, 0x01 + 1}",
        )
        for entries in invalid_entries:
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                MODULE.pack_mode(mode_table(entries))

    def test_rejects_out_of_range_bytes(self):
        for entries in ("{0x100, 0x00}", "{0x00, 0x100}"):
            with self.subTest(entries=entries), self.assertRaisesRegex(ValueError, "byte"):
                MODULE.pack_mode(mode_table(entries))

    def test_bounds_register_count(self):
        for count in (0, 513):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, "1 to 512"):
                MODULE.pack_mode(mode_table("{0x00, 0x01}," * count))
        self.assertEqual(len(MODULE.pack_mode(mode_table("{0x00, 0x01}," * 512))), 1036)

    def test_cli_rejects_invalid_input_without_overwriting_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "invalid mode.h"
            source.write_text("no table", encoding="utf-8")
            output = directory / "mode.bin"
            output.write_bytes(b"keep")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(source), str(output)],
                capture_output=True, timeout=5,
            )
            self.assertEqual(result.returncode, 1)
            self.assertNotIn(b"Traceback", result.stderr)
            self.assertEqual(output.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
