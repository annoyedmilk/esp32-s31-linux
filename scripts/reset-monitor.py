#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reset an ESP32-S31 board and capture its UART output to a log file."""

import argparse
import contextlib
import os
import select
import sys
import termios
import time
import tty
from collections.abc import Iterator
from typing import BinaryIO

import serial

CTRL_RBRACKET = b"\x1d"
SEEN_MAX = 4096


def now_name() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Reset an ESP32-S31 board and capture UART output"
    )
    parser.add_argument("--port", required=True, help="UART port to capture")
    parser.add_argument(
        "--reset-port",
        default="",
        help="optional separate port used only for DTR/RTS reset",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log-dir", default="logs")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--success-pattern", default="ESP32-S31 Linux / Buildroot")
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="stay connected as a terminal after the success pattern (Ctrl-] exits)",
    )
    return parser.parse_args()


def pulse_reset(ser: serial.Serial) -> None:
    # RTS drives EN (reset), as esptool does.  DTR drives GPIO0: keep it off.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)


def reset_board(ser: serial.Serial, args: argparse.Namespace, log: BinaryIO) -> None:
    reset_port = args.reset_port or args.port
    log.write(f"reset_pulse={reset_port}\n".encode())
    log.flush()
    if reset_port == args.port:
        pulse_reset(ser)
        return
    with serial.Serial(reset_port, args.baud, timeout=0.1) as reset_ser:
        pulse_reset(reset_ser)


@contextlib.contextmanager
def raw_stdin(interactive: bool) -> Iterator[int | None]:
    """Yield stdin in raw mode, or None when the session is not interactive."""
    if not interactive:
        yield None
        return
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        sys.stderr.write("console connected; press Ctrl-] to exit\r\n")
        sys.stderr.flush()
        yield fd
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        sys.stderr.write("\r\nconsole disconnected\n")
        sys.stderr.flush()


def forward_serial(ser: serial.Serial, log: BinaryIO) -> bytes:
    """Copy the waiting UART bytes to stdout and to the log, and return them."""
    data = ser.read(ser.in_waiting or 1)
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
    log.write(data)
    log.flush()
    return data


def forward_stdin(fd: int, ser: serial.Serial) -> bool:
    """Send the typed bytes to the UART; return False for Ctrl-]."""
    data = os.read(fd, 1024)
    if CTRL_RBRACKET in data:
        return False
    ser.write(data)
    return True


def capture(
    ser: serial.Serial,
    log: BinaryIO,
    pattern: bytes,
    deadline: float,
    stdin_fd: int | None,
) -> bool:
    """Copy the UART until the pattern, the deadline or Ctrl-].

    Without a pattern, copy until the deadline.  After the pattern, an
    interactive session stays connected until Ctrl-].
    """
    success = not pattern
    seen = b""
    readers = [ser.fileno()] + ([stdin_fd] if stdin_fd is not None else [])
    while time.monotonic() < deadline or (success and stdin_fd is not None):
        ready, _, _ = select.select(readers, [], [], 0.1)
        hit = False
        if ser.fileno() in ready:
            seen = (seen + forward_serial(ser, log))[-SEEN_MAX:]
            hit = not success and pattern in seen
            success = success or hit
        if hit and stdin_fd is None:
            return True
        if stdin_fd in ready and not forward_stdin(stdin_fd, ser):
            break
    return success


def run(args: argparse.Namespace, log: BinaryIO, deadline: float) -> bool:
    with serial.Serial(args.port, args.baud, timeout=0) as ser:
        ser.reset_input_buffer()
        if not args.no_reset:
            reset_board(ser, args, log)
        with raw_stdin(args.interactive and sys.stdin.isatty()) as stdin_fd:
            return capture(ser, log, args.success_pattern.encode(), deadline, stdin_fd)


def main() -> int:
    args = parse_args()
    os.makedirs(args.log_dir, exist_ok=True)
    path = os.path.join(args.log_dir, f"{now_name()}-reset-uart.log")
    deadline = time.monotonic() + args.timeout

    print(f"log={path}")
    with open(path, "wb") as log:
        log.write(
            f"port={args.port} reset_port={args.reset_port or args.port} "
            f"baud={args.baud} timeout={args.timeout}\n".encode()
        )
        log.flush()
        try:
            success = run(args, log, deadline)
        except serial.SerialException as exc:
            log.write(f"serial_error={exc}\n".encode())
            print(f"ERR serial {exc}")
            return 2
        except KeyboardInterrupt:
            log.write(b"interrupted\n")
            print("interrupted")
            return 130

    if success:
        print("capture=pass")
        return 0
    print("ERR timeout")
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
