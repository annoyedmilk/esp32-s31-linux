#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack the initramfs that lives in the flash partition.

Just enough of Buildroot's target to reach the rootfs on the card. The loader
reads the whole partition and checks for newc magic at offset 0, so the output
is always padded to the full partition size.
"""

from __future__ import annotations

import argparse
import os
import stat
import struct
from pathlib import Path


# Recovery toolkit, not a userspace: each of these costs ~120 bytes of header.
APPLETS = (
    "sh", "ash", "mount", "umount", "switch_root", "sleep", "echo", "cat",
    "ls", "mkdir", "dmesg", "blkid", "fdisk", "sync", "uname", "setsid",
    "cttyhack", "df", "free", "dd", "hexdump", "reboot", "poweroff",
)

DIRECTORIES = (".", "bin", "dev", "lib", "lib/firmware", "mnt", "proc", "run",
               "sys", "tmp")

# cfg80211 asks for these while the initramfs is still the root, so the copy
# on the card comes too late to answer.
FIRMWARE = ("regulatory.db", "regulatory.db.p7s")

PT_INTERP = 3


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


def elf_interpreter(path: Path) -> str | None:
    """The binary's PT_INTERP, or None when it is static.

    Read rather than guessed: musl's loader name carries the float ABI, which
    is -sf on this target.
    """
    blob = path.read_bytes()
    if blob[:4] != b"\x7fELF" or blob[4] != 1:
        raise SystemExit(f"{path} is not a 32-bit ELF")
    e_phoff, = struct.unpack_from("<I", blob, 0x1c)
    e_phentsize, e_phnum = struct.unpack_from("<HH", blob, 0x2a)
    for index in range(e_phnum):
        p_type, p_offset, _, _, p_filesz = struct.unpack_from(
            "<IIIII", blob, e_phoff + index * e_phentsize)
        if p_type == PT_INTERP:
            return blob[p_offset:p_offset + p_filesz].rstrip(b"\0").decode()
    return None


def build(target: Path, init: Path, size: int) -> bytes:
    archive = bytearray()
    ino = 1

    for directory in DIRECTORIES:
        add_entry(archive, directory, stat.S_IFDIR | 0o755, ino=ino)
        ino += 1

    busybox = target / "bin/busybox"
    if not busybox.is_file():
        raise SystemExit(f"no BusyBox in the Buildroot target: {busybox}")
    add_entry(archive, "bin/busybox", stat.S_IFREG | 0o755, busybox.read_bytes(), ino)
    ino += 1
    for applet in APPLETS:
        add_entry(archive, f"bin/{applet}", stat.S_IFLNK | 0o777, b"busybox", ino)
        ino += 1

    interp = elf_interpreter(busybox)
    if interp is not None:
        name = interp.lstrip("/")
        source = target / name
        if not source.exists():
            raise SystemExit(f"BusyBox wants {interp}, which the target lacks")
        # Buildroot ships the loader as a symlink; exec needs both halves.
        if source.is_symlink():
            link = os.readlink(source)
            real = (source.parent / link).resolve()
            add_entry(archive, f"{Path(name).parent}/{real.name}",
                      stat.S_IFREG | 0o755, real.read_bytes(), ino)
            ino += 1
            add_entry(archive, name, stat.S_IFLNK | 0o777, link.encode(), ino)
        else:
            add_entry(archive, name, stat.S_IFREG | 0o755, source.read_bytes(), ino)
        ino += 1

    for name in FIRMWARE:
        blob = target / "lib/firmware" / name
        if blob.exists():
            add_entry(archive, f"lib/firmware/{name}", stat.S_IFREG | 0o644,
                      blob.read_bytes(), ino)
            ino += 1

    add_entry(archive, "init", stat.S_IFREG | 0o755, init.read_bytes(), ino)
    add_entry(archive, "TRAILER!!!", 0, ino=ino + 1)

    if len(archive) > size:
        raise SystemExit(f"initramfs is {len(archive)} bytes; the partition is {size}")
    archive.extend(b"\0" * (size - len(archive)))
    return bytes(archive)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path, required=True,
                        help="Buildroot target/ directory")
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=lambda value: int(value, 0), default=0x200000)
    args = parser.parse_args()

    if not args.target.is_dir():
        parser.error(f"Buildroot target directory not found: {args.target}")
    if not args.init.is_file():
        parser.error(f"init not found: {args.init}")

    image = build(args.target, args.init, args.size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    used = image.rfind(b"TRAILER!!!") + len("TRAILER!!!")
    print(f"initramfs: {args.output} ({used} bytes used of {args.size})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
