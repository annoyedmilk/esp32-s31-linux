# ESP32-S31 Linux

Linux 7.1 and OpenSBI 1.9 bring-up for the ESP32-S31 Korvo-1, using 16 MiB
of octal PSRAM as Linux memory.

The verified boot chain is:

```text
ESP ROM -> ESP-IDF second stage -> loader -> OpenSBI -> Linux -> BusyBox initramfs
```

The loader initializes PSRAM, copies the exact Linux image length through the
cacheable aperture at `0x50000000`, zeroes its in-memory tail, copies the
initramfs to `0x50800000`,
installs the chip's single global PMP grant, and hands control to OpenSBI at
`0x2f000000`. OpenSBI provides SBI TIME through the machine timer and delivers
supervisor interrupts through the S31 CLIC.

The verified cache geometry is a 32 KiB, two-way instruction cache and a
64 KiB, two-way data cache, both with 64-byte lines. OpenSBI marks cached
PSRAM write-through so Linux does not observe stale page-table or allocator
state. Linux uses tickless idle at 100 Hz and enters native `wfi`; pending CLIC
interrupts wake the hart while the generic idle loop has `sstatus.SIE` clear.
Its SBI early console hands off to UART0 without retaining the boot console.

UART0 RX is interrupt-driven through CLIC slot 32. The S-mode CLIC trampoline
restores `scause.spil` immediately before `sret`; using `mret` leaves
`mintstatus.SIL` stuck and masks all subsequent supervisor interrupts.
OpenSBI initializes the supervisor timer slot once and only changes its pending
bit for timer delivery. Linux masks local timer and software interrupts through
the CLIC MMIO window because the standard S-mode interrupt CSRs are not
implemented on this hart.

## Hardware connections

The ESP32-S31 Korvo-1 board exposes two USB connections for debugging. You should use both:

- **On-board USB-to-Serial bridge**: This external bridge chip connects to the ESP32-S31 UART0 pins. It serves as the Linux `ttyS0` console and is specified via `SERIAL_PORT` during `make monitor`.
- **Native USB-Serial/JTAG port**: This is the native USB interface on the ESP32-S31 chip itself. It is specified via `FLASH_PORT` for flashing the binaries, starting JTAG debugging via `make openocd`, and it also mirrors early loader/OpenSBI firmware output.

On macOS, identify the external bridge with `ls /dev/cu.*`. The bridge normally
appears as a CP210x, CH34x, or FTDI device, and the on-chip port appears as
`/dev/cu.usbmodem*`. On Linux, identify them with `ls /dev/ttyUSB*` and
`ls /dev/ttyACM*`. The `/dev/ttyUSB*` device is normally the CP210x, CH34x,
or FTDI bridge; the `/dev/ttyACM*` device is the on-chip port.

## Host requirements

- Linux (Ubuntu/Debian) or macOS with Homebrew
- ESP-IDF at `~/esp/esp-idf`
- the ESP-IDF `riscv32-esp-elf` toolchain
- macOS: `brew install make gnu-sed findutils zig`
- Linux: `sudo apt install build-essential` (and obtain Zig from `ziglang.org/download`)

Zig supplies the RV32 musl userspace compiler for BusyBox. If
`riscv32-linux-musl-gcc` is installed, the build uses it instead. You can also
set `BUSYBOX_BIN=/path/to/static-rv32-busybox` to skip the BusyBox build.

Initialize dependencies and verify the host:

```sh
git submodule update --init --recursive
make check
make ports
```

`make ports` lists the serial devices and their USB descriptions. Connect both
board USB ports before running it. The on-chip flash/reset port is normally
`/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*` (Linux); the external bridge is
normally `/dev/cu.usbserial-*` (macOS) or `/dev/ttyUSB*` (Linux).

## Build and flash

```sh
make build
make flash FLASH_PORT=/dev/cu.usbmodem101
make monitor SERIAL_PORT=/dev/cu.usbserial-XXXX \
  RESET_PORT=/dev/cu.usbmodem101
```

The monitor waits up to 300 seconds for the BusyBox banner and then stays
attached as an interactive terminal. Press Enter if the shell prompt is not
visible. Press `Ctrl-]` to disconnect. Every session is copied verbatim to
`logs/`.

If the external UART's RTS line resets the board, `RESET_PORT` can be omitted.
If you do not want the monitor to reset a running system, invoke the script
directly with `--no-reset`:

```sh
source ~/esp/esp-idf/export.sh
python scripts/reset-monitor.py --port /dev/cu.usbserial-XXXX \
  --baud 115200 --no-reset --interactive
```

A successful boot displays:

```text
=== ESP32-S31 Linux / BusyBox ===
```

and presents an interactive shell on the external UART.

## Flash layout

| Offset | Contents |
| ---: | --- |
| `0x00002000` | ESP-IDF second-stage bootloader |
| `0x00008000` | partition table |
| `0x00020000` | ESP32-S31 Linux loader |
| `0x00220000` | OpenSBI fw_jump |
| `0x002a0000` | Linux Image |
| `0x00a1fff4` | Linux size and CRC manifest |
| `0x00a20000` | 2 MiB initramfs partition |

## Debugging

Start OpenOCD on the on-chip JTAG connection:

```sh
make openocd
```

Then connect the ESP RISC-V GDB to port 3333 using
`build/opensbi.elf`, `build/linux/vmlinux`, or
`build/bootloader/s31-linux-loader.elf` as appropriate.

Boot logs are written under `logs/` and ignored by Git.

## Current limitations

- one CPU is enabled;
- the single PMP entry is a locked global RWX grant, so OpenSBI domain
  isolation is intentionally unavailable;
- APM/PMS permissions are broad bring-up grants;
- hardware reset/shutdown through SBI is not implemented;
- networking and most board peripherals are not enabled yet.
