#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests of thread-relay: a pseudo-terminal is the RCP.

test_relay.py ./thread-relay-host
"""

import os
import pty
import select
import socket
import subprocess
import sys
import time

ALL_BYTES = bytes(range(256))


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


def read_fd(fd: int, size: int, timeout: float = 2) -> bytes:
    data = b""
    deadline = time.monotonic() + timeout
    while len(data) < size and time.monotonic() < deadline:
        if select.select([fd], [], [], 0.05)[0]:
            data += os.read(fd, size - len(data))
    return data


def read_sock(s: socket.socket, size: int, timeout: float = 2) -> bytes:
    data = b""
    deadline = time.monotonic() + timeout
    while len(data) < size and time.monotonic() < deadline:
        if select.select([s], [], [], 0.05)[0]:
            chunk = s.recv(size - len(data))
            if not chunk:
                break
            data += chunk
    return data


class Relay:
    def __init__(self, binary: str) -> None:
        # Keep the slave open: a write to the master fails while no one has it.
        self.master, self.slave = pty.openpty()
        self.port = free_port()
        self.proc = subprocess.Popen(
            [binary, "-p", str(self.port), os.ttyname(self.slave)],
            stderr=subprocess.DEVNULL,
        )

    def stop(self) -> None:
        self.proc.kill()
        self.proc.wait()
        os.close(self.master)
        os.close(self.slave)


def test_both_directions(r: Relay) -> None:
    with connect(r.port) as c:
        time.sleep(0.1)
        os.write(r.master, ALL_BYTES)
        assert read_sock(c, 256) == ALL_BYTES, "RCP to client"
        c.sendall(ALL_BYTES)
        assert read_fd(r.master, 256) == ALL_BYTES, "client to RCP"


def test_new_client_replaces_old(r: Relay) -> None:
    old = connect(r.port)
    time.sleep(0.1)
    with connect(r.port) as new:
        time.sleep(0.1)
        assert read_sock(old, 1) == b"", "old client not closed"
        os.write(r.master, b"frame")
        assert read_sock(new, 5) == b"frame", "new client gets no data"
    old.close()


def test_no_client_drops_frames(r: Relay) -> None:
    os.write(r.master, b"old")
    time.sleep(0.2)
    with connect(r.port) as c:
        time.sleep(0.1)
        os.write(r.master, b"new")
        assert read_sock(c, 6, timeout=0.5) == b"new", "frames from before were kept"


def test_device_end_closes_client(binary: str) -> None:
    r = Relay(binary)
    with connect(r.port) as c:
        time.sleep(0.1)
        os.close(r.master)
        os.close(r.slave)
        assert read_sock(c, 1) == b"", "client not closed when the device ends"
        time.sleep(0.5)
        assert r.proc.poll() is None, "relay ended with the device"
    r.proc.kill()
    r.proc.wait()


def main() -> int:
    binary = sys.argv[1]
    failed = 0
    for test in (
        test_both_directions,
        test_new_client_replaces_old,
        test_no_client_drops_frames,
    ):
        r = Relay(binary)
        try:
            test(r)
            print(f"ok   {test.__name__}")
        except (AssertionError, OSError) as e:
            print(f"FAIL {test.__name__}: {e}")
            failed += 1
        finally:
            r.stop()
    try:
        test_device_end_closes_client(binary)
        print("ok   test_device_end_closes_client")
    except (AssertionError, OSError) as e:
        print(f"FAIL test_device_end_closes_client: {e}")
        failed += 1
    print(f"{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
