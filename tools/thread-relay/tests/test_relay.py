#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests of thread-relay: a pseudo-terminal is the RCP.

test_relay.py ./thread-relay-host
"""

from __future__ import annotations

import os
import pty
import select
import socket
import subprocess
import sys
import time
import unittest

ALL_BYTES = bytes(range(256))
BINARY = ""


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def connect(port: int) -> socket.socket:
    deadline = time.monotonic() + 5
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=2)
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.05)


def read(f: int | socket.socket, size: int, timeout: float = 2) -> bytes:
    """Read up to size bytes from a file descriptor or a socket."""
    fd = f if isinstance(f, int) else f.fileno()
    data = b""
    deadline = time.monotonic() + timeout
    while len(data) < size and time.monotonic() < deadline:
        if select.select([fd], [], [], 0.05)[0]:
            chunk = os.read(fd, size - len(data))
            if not chunk:
                break
            data += chunk
    return data


class RelayTest(unittest.TestCase):
    def setUp(self) -> None:
        # Keep the slave open: a write to the master fails while no one has it.
        self.master, self.slave = pty.openpty()
        self.port = free_port()
        self.proc = subprocess.Popen(
            [BINARY, "-p", str(self.port), os.ttyname(self.slave)],
            stderr=subprocess.DEVNULL,
        )

    def close_pty(self) -> None:
        for fd in (self.master, self.slave):
            if fd >= 0:
                os.close(fd)
        self.master = self.slave = -1

    def tearDown(self) -> None:
        self.proc.kill()
        self.proc.wait()
        self.close_pty()

    def test_both_directions(self) -> None:
        with connect(self.port) as c:
            time.sleep(0.1)
            os.write(self.master, ALL_BYTES)
            self.assertEqual(read(c, 256), ALL_BYTES, "RCP to client")
            c.sendall(ALL_BYTES)
            self.assertEqual(read(self.master, 256), ALL_BYTES, "client to RCP")

    def test_new_client_replaces_old(self) -> None:
        with connect(self.port) as old:
            time.sleep(0.1)
            with connect(self.port) as new:
                time.sleep(0.1)
                self.assertEqual(read(old, 1), b"", "old client not closed")
                os.write(self.master, b"frame")
                self.assertEqual(read(new, 5), b"frame", "new client gets no data")

    def test_no_client_drops_frames(self) -> None:
        os.write(self.master, b"old")
        time.sleep(0.2)
        with connect(self.port) as c:
            time.sleep(0.1)
            os.write(self.master, b"new")
            self.assertEqual(
                read(c, 6, timeout=0.5), b"new", "frames from before were kept"
            )

    def test_device_end_closes_client(self) -> None:
        with connect(self.port) as c:
            time.sleep(0.1)
            self.close_pty()
            self.assertEqual(read(c, 1), b"", "client not closed when the device ends")
            time.sleep(0.5)
            self.assertIsNone(self.proc.poll(), "relay ended with the device")


if __name__ == "__main__":
    BINARY = sys.argv.pop(1)
    unittest.main()
