#!/usr/bin/env python3
import argparse
import os
import select
import sys
import termios
import time
import tty

import serial


def now_name():
    return time.strftime("%Y%m%d-%H%M%S")


def pulse_reset(ser):
    # ESP USB-Serial/JTAG follows esptool-style active-low EN on RTS.
    # DTR controls GPIO0 on common boards; keep it inactive for normal boot.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)


def main():
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
    parser.add_argument("--success-pattern", default="ESP32-S31 Linux / BusyBox")
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="stay connected as a terminal after the success pattern (Ctrl-] exits)",
    )
    args = parser.parse_args()

    os.makedirs(args.log_dir, exist_ok=True)
    path = os.path.join(args.log_dir, f"{now_name()}-reset-uart.log")
    deadline = time.monotonic() + args.timeout
    success = not args.success_pattern

    print(f"log={path}")
    with open(path, "wb") as log:
        header = (
            f"port={args.port} reset_port={args.reset_port or args.port} "
            f"baud={args.baud} timeout={args.timeout}\n"
        ).encode()
        log.write(header)
        log.flush()
        try:
            with serial.Serial(args.port, args.baud, timeout=0) as ser:
                ser.reset_input_buffer()
                if not args.no_reset:
                    reset_port = args.reset_port or args.port
                    log.write(f"reset_pulse={reset_port}\n".encode())
                    log.flush()
                    if reset_port == args.port:
                        pulse_reset(ser)
                    else:
                        with serial.Serial(
                            reset_port, args.baud, timeout=0.1
                        ) as reset_ser:
                            pulse_reset(reset_ser)
                interactive = args.interactive and sys.stdin.isatty()
                stdin_fd = sys.stdin.fileno() if interactive else None
                old_tty = termios.tcgetattr(stdin_fd) if interactive else None
                seen = bytearray()
                try:
                    if interactive:
                        tty.setraw(stdin_fd)
                        sys.stderr.write("console connected; press Ctrl-] to exit\r\n")
                        sys.stderr.flush()
                    while True:
                        if not success and time.monotonic() >= deadline:
                            break
                        readers = [ser.fileno()]
                        if interactive:
                            readers.append(stdin_fd)
                        ready, _, _ = select.select(readers, [], [], 0.1)
                        if ser.fileno() in ready:
                            data = ser.read(ser.in_waiting or 1)
                            if data:
                                sys.stdout.buffer.write(data)
                                sys.stdout.buffer.flush()
                                log.write(data)
                                log.flush()
                                if not success:
                                    seen.extend(data)
                                    if len(seen) > 4096:
                                        del seen[:-4096]
                                    pattern = args.success_pattern.encode()
                                    if pattern and pattern in seen:
                                        success = True
                                        if not interactive:
                                            break
                        if interactive and stdin_fd in ready:
                            data = os.read(stdin_fd, 1024)
                            if b"\x1d" in data:
                                break
                            if data:
                                ser.write(data)
                finally:
                    if old_tty is not None:
                        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_tty)
                        sys.stderr.write("\r\nconsole disconnected\n")
                        sys.stderr.flush()
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
