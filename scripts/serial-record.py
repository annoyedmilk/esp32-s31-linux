#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record a UART continuously and expose a PTY for console access."""

import argparse
import errno
import fcntl
import json
import os
import pty
import select
import signal
import sys
import time
import tty
from pathlib import Path

import serial

MAX_BYTES = 15_000_000
LOG_FILES = 3
CHUNK_SIZE = 4096


class RollingLog:
    """Keep raw UART output in three files, newest in uart.log."""

    def __init__(self, directory: Path, max_bytes: int) -> None:
        self.paths = [directory / "uart.log"] + [
            directory / f"uart.log.{index}" for index in range(1, LOG_FILES)
        ]
        self.part_bytes = max_bytes // LOG_FILES
        # Recover a bounded tail even if an earlier run used a larger limit.
        for index, path in enumerate(self.paths):
            if path.exists():
                path.chmod(0o600)
                if path.stat().st_size > self.part_bytes:
                    # Older files would leave gaps in the retained byte stream.
                    for older in self.paths[index + 1:]:
                        older.unlink(missing_ok=True)
                    with path.open("r+b") as stream:
                        stream.seek(-self.part_bytes, os.SEEK_END)
                        tail = stream.read(self.part_bytes)
                        stream.seek(0)
                        stream.write(tail)
                        stream.truncate()
        self.stream = self.paths[0].open("ab", buffering=0)
        self.size = self.stream.tell()

    def rotate(self) -> None:
        self.stream.close()
        self.paths[-1].unlink(missing_ok=True)
        for index in range(LOG_FILES - 2, -1, -1):
            if self.paths[index].exists():
                self.paths[index].replace(self.paths[index + 1])
        self.stream = self.paths[0].open("ab", buffering=0)
        self.size = 0

    def write(self, data: bytes) -> None:
        while data:
            if self.size == self.part_bytes:
                self.rotate()
            count = min(len(data), self.part_bytes - self.size)
            # FileIO is unbuffered; account for a short write explicitly.
            written = self.stream.write(data[:count])
            if not written:
                raise OSError("UART log write made no progress")
            self.size += written
            data = data[written:]

    def close(self) -> None:
        self.stream.close()


class Recorder:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.running = True
        self.port = None
        self.connected = False
        self.error = None
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        os.set_blocking(self.master, False)
        self.console = args.log_dir / "console"
        self.status = args.log_dir / "status.json"
        self.pending = b""
        self.dropped = 0
        self.started = time.strftime("%Y-%m-%dT%H:%M:%S%z")

    def stop(self, signum: int, frame: object) -> None:
        self.running = False

    def write_status(self) -> None:
        data = {
            "pid": os.getpid(),
            "port": self.args.port,
            "baud": self.args.baud,
            "started": self.started,
            "connected": self.connected,
            "console": str(self.console.absolute()),
            "max_log_bytes": self.args.max_bytes,
            "console_bytes_dropped": self.dropped,
            "last_serial_error": self.error,
        }
        temporary = self.status.with_suffix(".tmp")
        temporary.write_text(json.dumps(data, indent=2) + "\n")
        temporary.replace(self.status)

    def connect(self) -> None:
        # Set control lines before open: never intentionally pulse reset/BOOT.
        port = serial.Serial(
            port=None, baudrate=self.args.baud, timeout=0,
            write_timeout=1, exclusive=True,
        )
        port.dtr = False
        port.rts = False
        port.port = self.args.port
        try:
            port.open()
        except (OSError, serial.SerialException):
            port.close()
            raise
        self.port = port
        self.connected = True
        self.error = None
        self.write_status()

    def disconnect(self) -> None:
        if self.port is not None:
            self.port.close()
            self.port = None
        self.connected = False
        self.write_status()

    def forward_console(self) -> None:
        data = os.read(self.master, CHUNK_SIZE)
        if self.port is not None and data:
            written = self.port.write(data)
            if written != len(data):
                raise serial.SerialException("short console write")

    def run(self, log: RollingLog) -> None:
        self.console.unlink(missing_ok=True)
        self.console.symlink_to(os.ttyname(self.slave))
        self.write_status()
        retry_at = 0.0
        try:
            while self.running:
                if self.port is None and time.monotonic() >= retry_at:
                    try:
                        self.connect()
                    except (OSError, serial.SerialException) as error:
                        self.error = str(error)
                        self.write_status()
                        retry_at = time.monotonic() + 1
                readers = [self.master]
                if self.port is not None:
                    readers.append(self.port.fileno())
                ready, writable, _ = select.select(
                    readers, [self.master] if self.pending else [], [], 0.5,
                )
                try:
                    if self.master in ready:
                        self.forward_console()
                    if self.port is not None and self.port.fileno() in ready:
                        data = self.port.read(CHUNK_SIZE)
                        # Save before forwarding: a slow terminal cannot lose logs.
                        log.write(data)
                        self.pending += data
                        if len(self.pending) > CHUNK_SIZE * 4:
                            self.dropped += len(self.pending) - CHUNK_SIZE * 4
                            self.pending = self.pending[-CHUNK_SIZE * 4:]
                            self.write_status()
                    if self.master in writable:
                        try:
                            count = os.write(self.master, self.pending)
                            self.pending = self.pending[count:]
                        except BlockingIOError:
                            pass
                except serial.SerialException as error:
                    self.error = str(error)
                    self.disconnect()
                    retry_at = time.monotonic() + 1
        finally:
            self.disconnect()
            self.console.unlink(missing_ok=True)
            os.close(self.master)
            os.close(self.slave)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="physical UART; one recorder per port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log-dir", type=Path, default=Path("logs/serial"))
    parser.add_argument(
        "--max-bytes", type=int, default=MAX_BYTES,
        help="total UART log limit in bytes (at most 15000000)",
    )
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if not LOG_FILES <= args.max_bytes <= MAX_BYTES:
        parser.error("--max-bytes must be between 3 and 15000000")
    return args


def main() -> int:
    args = parse_args()
    os.umask(0o077)
    args.log_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    args.log_dir.chmod(0o700)
    # Keep the lock for the process lifetime, including UART reconnects.
    with (args.log_dir / "recorder.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno not in (errno.EAGAIN, errno.EACCES):
                raise
            print("a recorder already owns this log directory", file=sys.stderr)
            return 1
        recorder = Recorder(args)
        signal.signal(signal.SIGTERM, recorder.stop)
        signal.signal(signal.SIGINT, recorder.stop)
        log = RollingLog(args.log_dir, args.max_bytes)
        try:
            print(f"console={recorder.console} logs={args.log_dir}", flush=True)
            recorder.run(log)
        finally:
            log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
