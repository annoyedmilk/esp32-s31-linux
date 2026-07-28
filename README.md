# ESP32-S31 Linux

Linux 7.1 and OpenSBI 1.9 bring-up for the ESP32-S31 Korvo-1, using 16 MiB
of octal PSRAM as Linux memory.

The verified boot chain is:

```text
ESP ROM -> ESP-IDF second stage -> loader -> OpenSBI -> Linux -> BusyBox initramfs
```

The ESP-IDF second stage brings up PSRAM before `app_main` runs. The loader
verifies that, copies the exact Linux image length through the cacheable
aperture at `0x50000000` after checking the image's size and CRC manifest,
zeroes its in-memory tail, copies the initramfs to `0x50800000`, installs the
chip's single global PMP grant, and hands control to OpenSBI at `0x2f000000`.
OpenSBI provides SBI TIME through the machine timer and delivers supervisor
interrupts through the S31 CLIC.

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

The Korvo-1's 800x480 RGB LCD has a loader-only proof of concept when USB
Serial/JTAG is disabled. The loader brings up the LCD_CAM RGB panel with a
fixed RGB565 frame buffer at the top of PSRAM (`0x50F40000`), rebuilds the
circular AXI DMA descriptor chain at `0x2F079000` in upper SRAM so the OpenSBI
copy cannot clobber it, silences the LCD and DMA interrupt sources, and paints
color bars. Linux does not yet reserve or expose this frame buffer, so the
display is not currently a Linux framebuffer device. The LCD data bus uses
GPIO33 and GPIO34, which are also the native USB Serial/JTAG D-/D+ pins, so
the loader skips display initialization whenever the ESP-IDF USB Serial/JTAG
console is enabled.

The board's USB Type-A connector is exposed as a Linux high-speed USB host.
An ESP32-S31 PHY driver performs the ESP-IDF clock, reset, UTMI, and host
pull-down sequence, then the standard DWC2 host controller handles the bus.
USB HID and evdev support are built in, so keyboards and mice appear under
`/dev/input`. The controller currently uses PIO rather than DMA because Linux
memory resides in cached PSRAM and cache maintenance is not implemented yet.

## SD card

The Korvo-1 microSD slot sits on SDMMC slot 0 (dedicated pads GPIO20–25,
routed via IO MUX; the board's unpopulated SPI NAND footprint shares these
pins). The loader powers the slot through the board's active-low enable on
GPIO39, derives the 50 MHz controller clock from the already-running MPLL,
releases the module reset, hands the pads to the SD host, and reports
card-present/not-write-protected through the GPIO matrix constant inputs,
since the slot has no CD/WP contacts.

Linux drives the controller with the stock `dw_mmc` driver (the S31 SDHOST
is a Synopsys DesignWare MSHC). Patch
`0003-mmc-dw_mmc-add-esp32s31-support.patch` identifies the SoC integration
and forces PIO transfers: the controller's internal DMA is not coherent with
the CPU caches and the hart implements no Zicbom cache management, so DMA
descriptor writeback and card reads would observe stale cache lines. FAT
(VFAT) and ext4 are enabled; the initramfs mounts the first partition, or a
whole-card filesystem when there is no partition table, on `/mnt/sd` during
boot. Cards can also be mounted manually:

```sh
mount /dev/mmcblk0p1 /mnt/sd
```

Detection is the success criterion: a working card enumerates as
`/dev/mmcblk0`. An empty card carries no partition table or filesystem, so
its boot-time mount fails and init prints
`SD: /dev/mmcblk0 present but mount failed`. That message is the expected
result until a filesystem is written to the card.

## Hardware connections

The ESP32-S31 Korvo-1 has two Type-C connectors, a Type-A host connector, and
native USB Serial/JTAG signals on the LCD expansion connector:

- **Power Type-C**: Supplies power only; it has no data connection.
- **UART Type-C**: Connects through the on-board CP2102N bridge to UART0. It
  can flash the board and provides the Linux `ttyS0` console.
- **Native USB Serial/JTAG breakout**: The Korvo-1 has no dedicated native
  USB Type-C connector. Connect USB white/D- to GPIO33, green/D+ to GPIO34,
  and black/GND to board ground. Leave USB red/5 V disconnected when the
  board is already powered through Type-C. This interface appears as
  Espressif VID:PID `303a:1001`; it can flash the board and is used by
  `make openocd`.
- **USB Type-A host**: This connector is wired to the ESP32-S31 USB 2.0 OTG
  high-speed peripheral and supplies attached devices from the board's
  current-limited 5 V VBUS path. It is independent of the two debug ports.

On macOS, the CP2102N normally appears as `/dev/cu.usbserial-*` and the native
USB Serial/JTAG breakout as `/dev/cu.usbmodem*`.

## Host requirements

- macOS with Homebrew
- ESP-IDF at `~/esp/esp-idf`
- the ESP-IDF `riscv32-esp-elf` toolchain
- `brew install make gnu-sed findutils zig`

Zig supplies the RV32 musl userspace compiler for BusyBox. If
`riscv32-linux-musl-gcc` is installed, the build uses it instead. You can also
set `BUSYBOX_BIN=/path/to/static-rv32-busybox` to skip the BusyBox build.

Initialize dependencies and verify the host:

```sh
git submodule update --init --recursive
make check
make ports
```

`make ports` lists the serial devices and USB descriptions. The CP2102N UART
is normally `/dev/cu.usbserial-*`; a wired native USB Serial/JTAG breakout is
normally `/dev/cu.usbmodem*`.

## Build and flash

```sh
make build
make flash FLASH_PORT=/dev/cu.usbserial-XXXX
make monitor SERIAL_PORT=/dev/cu.usbserial-XXXX
```

`FLASH_PORT` names the esptool target and may be either the native USB
Serial/JTAG (`/dev/cu.usbmodem*`) or the CP2102N UART bridge
(`/dev/cu.usbserial-*`). `SERIAL_PORT` names the external UART that carries
the Linux console. `make build` reuses the patched kernel tree under
`build/` and regenerates it when the patch series changes; run `make clean`
after updating the `external/` submodules.

The monitor waits up to 300 seconds (`BOOT_TIMEOUT`) for the BusyBox banner
and then stays attached as an interactive terminal. Press Enter if the shell
prompt is not visible. Press `Ctrl-]` to disconnect. Every session is copied
verbatim to `logs/`.

The monitor pulses RTS on `SERIAL_PORT` to reset the board before capturing.
If that adapter cannot reset the board, set `RESET_PORT=/dev/cu.X` to send
the pulse through a second port, such as the native USB Serial/JTAG. To
attach to a running system without resetting it, invoke the script directly
with `--no-reset`:

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

To verify a keyboard or mouse on the Type-A connector after boot:

```sh
dmesg | tail -n 30
ls -l /sys/bus/usb/devices
cat /proc/bus/input/devices
ls -l /dev/input
```

The DWC2 root hub should be present before a device is connected. Plugging in
a HID device should add a USB device and an `event*` input node.

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

With the GPIO33/34 native USB breakout connected, start OpenOCD:

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
- USB host transfers use PIO and only the HID class is enabled; mass storage
  and USB networking are not enabled yet;
- networking and most board peripherals are not enabled yet.
