# SPDX-License-Identifier: MIT
"""Exercise log retention and the recorder through simulated UARTs."""

import importlib.util
import json
import os
import pty
import select
import subprocess
import sys
import tempfile
import time
import tty
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "serial-record.py"
SPEC = importlib.util.spec_from_file_location("serial_record", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def log_bytes(directory):
    return b"".join(
        path.read_bytes() for path in (
            directory / "uart.log.2", directory / "uart.log.1",
            directory / "uart.log",
        ) if path.exists()
    )


def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out waiting for recorder")


class RollingLogTests(unittest.TestCase):
    def test_rotation_and_restart_preserve_bounded_recent_tail(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            data = bytes(range(256)) * 7
            logger = MODULE.RollingLog(directory, 300)
            for offset in range(0, len(data), 37):
                logger.write(data[offset:offset + 37])
                saved = log_bytes(directory)
                self.assertLessEqual(len(saved), 300)
                self.assertTrue(data[:offset + 37].endswith(saved))
            logger.close()
            saved = log_bytes(directory)
            self.assertGreaterEqual(len(saved), 200)
            logger = MODULE.RollingLog(directory, 120)
            self.assertLessEqual(len(log_bytes(directory)), 120)
            self.assertTrue(saved.endswith(log_bytes(directory)))
            logger.write(b"after restart")
            logger.close()
            self.assertTrue(log_bytes(directory).endswith(b"after restart"))
            self.assertLessEqual(len(log_bytes(directory)), 120)


class RecorderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.master, self.slave = pty.openpty()
        self.addCleanup(self.close_uart)
        tty.setraw(self.slave)
        self.device = self.directory / "device"
        self.device.symlink_to(os.ttyname(self.slave))
        self.logs = self.directory / "logs"
        self.command = [
            sys.executable, str(SCRIPT), "--port", str(self.device),
            "--log-dir", str(self.logs), "--max-bytes", "60000",
        ]
        self.process = subprocess.Popen(
            self.command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        self.addCleanup(self.stop_recorder)
        wait_for(lambda: self.status().get("connected"))

    def status(self):
        try:
            return json.loads((self.logs / "status.json").read_text())
        except FileNotFoundError:
            return {}

    def close_uart(self):
        os.close(self.master)
        os.close(self.slave)

    def stop_recorder(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            _, errors = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
            self.fail("recorder did not stop after SIGTERM")
        self.assertEqual(self.process.returncode, 0, errors.decode())

    def test_console_and_logging_do_not_transmit_on_connect(self):
        self.assertFalse(select.select([self.master], [], [], 0.1)[0])
        console = os.open(self.logs / "console", os.O_RDWR | os.O_NOCTTY)
        try:
            tty.setraw(console)
            os.write(self.master, b"Kernel panic - test only\r\n")
            wait_for(lambda: b"Kernel panic" in log_bytes(self.logs))
            self.assertTrue(select.select([console], [], [], 2)[0])
            self.assertEqual(os.read(console, 100), b"Kernel panic - test only\r\n")
            os.write(console, b"uname -a\n")
            self.assertTrue(select.select([self.master], [], [], 2)[0])
            self.assertEqual(os.read(self.master, 100), b"uname -a\n")
            self.assertNotIn(b"uname", log_bytes(self.logs))
        finally:
            os.close(console)
        self.assertEqual((self.logs / "uart.log").stat().st_mode & 0o777, 0o600)

    def test_slow_console_does_not_block_log_rotation(self):
        payload = b"0123456789abcdef" * 8192
        for offset in range(0, len(payload), 4096):
            os.write(self.master, payload[offset:offset + 4096])
        wait_for(lambda: self.status().get("console_bytes_dropped", 0) > 0)
        marker = b"\nEND-OF-CAPTURE\n"
        os.write(self.master, marker)
        wait_for(lambda: log_bytes(self.logs).endswith(marker))
        self.assertLessEqual(len(log_bytes(self.logs)), 60000)
        self.assertTrue(payload.endswith(log_bytes(self.logs)[:-len(marker)]))

    def test_reconnect_and_duplicate_recorder(self):
        duplicate = subprocess.run(self.command, capture_output=True, timeout=3)
        self.assertEqual(duplicate.returncode, 1)
        self.assertIn(b"already owns", duplicate.stderr)
        os.close(self.master)
        os.close(self.slave)
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.device.unlink()
        self.device.symlink_to(os.ttyname(self.slave))
        wait_for(lambda: not self.status().get("connected"))
        wait_for(lambda: self.status().get("connected"))
        os.write(self.master, b"reconnected\n")
        wait_for(lambda: log_bytes(self.logs).endswith(b"reconnected\n"))

    def test_second_directory_cannot_claim_same_uart(self):
        command = self.command.copy()
        command[command.index("--log-dir") + 1] = str(self.directory / "other")
        duplicate = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        try:
            def busy():
                path = self.directory / "other" / "status.json"
                return path.exists() and json.loads(path.read_text()).get(
                    "last_serial_error",
                )

            wait_for(busy)
            self.assertTrue(self.status()["connected"])
            os.write(self.master, b"primary still owns UART\n")
            wait_for(lambda: log_bytes(self.logs).endswith(b"primary still owns UART\n"))
        finally:
            duplicate.terminate()
            _, errors = duplicate.communicate(timeout=5)
            self.assertEqual(duplicate.returncode, 0, errors.decode())


if __name__ == "__main__":
    unittest.main()
