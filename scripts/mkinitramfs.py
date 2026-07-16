#!/usr/bin/env python3
"""Build the fixed-size newc initramfs stored in the flash partition."""

import argparse
import stat
from pathlib import Path


APPLETS = (
    "ash", "cat", "chmod", "clear", "cp", "cttyhack", "dmesg", "echo",
    "false", "free", "kill", "ln", "ls", "mkdir", "mknod", "mount",
    "mountpoint", "ps", "pwd", "reboot", "rm", "rmdir", "sh", "sleep",
    "setsid", "sync", "true", "umount", "uname",
)


def align4(data: bytearray) -> None:
    data.extend(b"\0" * (-len(data) & 3))


def add_entry(archive: bytearray, name: str, mode: int, data: bytes = b"",
              ino: int = 1) -> None:
    encoded_name = name.encode() + b"\0"
    fields = (
        ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0,
        len(encoded_name), 0,
    )
    archive.extend(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
    archive.extend(encoded_name)
    align4(archive)
    archive.extend(data)
    align4(archive)


def build(busybox: Path, init: Path, size: int) -> bytes:
    archive = bytearray()
    ino = 1

    for directory in (".", "bin", "dev", "dev/pts", "proc", "run", "sys", "tmp"):
        add_entry(archive, directory, stat.S_IFDIR | 0o755, ino=ino)
        ino += 1

    add_entry(archive, "bin/busybox", stat.S_IFREG | 0o755, busybox.read_bytes(), ino)
    ino += 1
    for applet in APPLETS:
        add_entry(archive, f"bin/{applet}", stat.S_IFLNK | 0o777,
                  b"busybox", ino)
        ino += 1

    add_entry(archive, "init", stat.S_IFREG | 0o755, init.read_bytes(), ino)
    add_entry(archive, "TRAILER!!!", 0, ino=ino + 1)

    if len(archive) > size:
        raise ValueError(f"initramfs is {len(archive)} bytes; partition is {size} bytes")
    archive.extend(b"\0" * (size - len(archive)))
    return bytes(archive)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--busybox", type=Path, required=True)
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=lambda value: int(value, 0), default=0x200000)
    args = parser.parse_args()

    if not args.busybox.is_file():
        parser.error(f"BusyBox binary not found: {args.busybox}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build(args.busybox, args.init, args.size))
    print(f"initramfs: {args.output} ({args.size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
