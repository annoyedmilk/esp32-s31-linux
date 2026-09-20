# ESP32-S31 Linux

Linux 7.1 and OpenSBI 1.9 bring-up for the ESP32-S31 Korvo-1, using 16 MiB
of octal PSRAM as Linux memory.

The verified boot chain is:

```text
ESP ROM -> ESP-IDF second stage -> loader -> OpenSBI -> Linux -> initramfs -> Buildroot rootfs (SD)
```

The two harts are split. Hart 0 never leaves M-mode: it runs the ESP-IDF
loader, which stays resident afterwards as the firmware that owns the WLAN
modem. Linux gets hart 1 to itself.

The ESP-IDF second stage brings up PSRAM before `app_main` runs. The loader
verifies that, copies the exact Linux image length through the cacheable
aperture at `0x50000000` after checking the image's size and CRC manifest,
zeroes its in-memory tail, copies the initramfs to `0x50800000` and OpenSBI to
`0x50e00000`, brings up Wi-Fi, and then releases hart 1 into OpenSBI. OpenSBI
provides SBI TIME through the machine timer and delivers supervisor interrupts
through the S31 CLIC.

OpenSBI runs from reserved PSRAM rather than internal SRAM because the resident
firmware links its own data at the bottom of SRAM. Two pieces of per-hart state
have to be established before hart 1 can get anywhere: the loader's entry stub
grants it a PMA entry covering PSRAM, without which OpenSBI faults on the first
write to its own BSS, and OpenSBI hands the CLIC's Interrupt Matrix inputs to
S-mode. An input whose `clicintattr.MODE` is not supervisor is invisible
through the supervisor register window -- it reads as zero and drops writes --
so without that step Linux silently receives no external interrupt at all.

The verified cache geometry is a 32 KiB, two-way instruction cache and a
64 KiB, two-way data cache, both with 64-byte lines. OpenSBI marks cached
PSRAM write-through so Linux does not observe stale page-table or allocator
state. Only the flash and PSRAM apertures are cached at all; internal SRAM
and MMIO bypass the cache.

No bus master is coherent with the data cache and the hart implements no
Zicbom, so DMA relies on the cache controller's MMIO sync engine, driven by
`drivers/cache/esp32s31-cache.c` through the RISC-V non-standard cache-ops
hook. Two consequences shape every DMA path here. Streaming mappings work
normally, but because there is no SWIOTLB to bounce a buffer that shares a
cache line with an unrelated allocation, `setup_arch()` declares non-coherent
support early enough that kmalloc keeps 64-byte alignment. Coherent
allocations cannot come from PSRAM at all: it has no uncached alias, and Sv32
page tables carry no cacheability attribute, so `dma_alloc_coherent()` is
served from a 64 KiB `shared-dma-pool` in internal SRAM at `0x2f040000`,
which is coherent by construction. Linux uses tickless idle at 100 Hz and enters native `wfi`; pending CLIC
interrupts wake the hart while the generic idle loop has `sstatus.SIE` clear.
Its SBI early console hands off to UART0 without retaining the boot console.

UART0 RX is interrupt-driven through CLIC slot 32. The S-mode CLIC trampoline
restores `scause.spil` immediately before `sret`; using `mret` leaves
`mintstatus.SIL` stuck and masks all subsequent supervisor interrupts.
OpenSBI initializes the supervisor timer slot once and only changes its pending
bit for timer delivery. Linux masks local timer and software interrupts through
the CLIC MMIO window because the standard S-mode interrupt CSRs are not
implemented on this hart.

The Korvo-1's 800x480 RGB LCD is a Linux frame buffer. The loader brings up
the LCD_CAM RGB panel with a fixed RGB565 frame buffer at the top of PSRAM
(`0x50F40000`), rebuilds the circular AXI DMA descriptor chain at `0x2F079000`
in upper SRAM so the OpenSBI copy cannot clobber it, silences the LCD and DMA
interrupt sources, and paints color bars; its DMA keeps scanning that buffer
out across the handoff. Linux reserves it as `fb_region` and describes it as a
`simple-framebuffer` under `/chosen`, so it appears as `/dev/fb0` with `fbcon`
on the virtual terminals. `console=tty0` puts the kernel log on the panel, at
the cost of about a second of boot time spent scrolling PSRAM, and the
rootfs runs a second getty on `/dev/tty1` that a USB keyboard drives
directly. Userspace boot output does not appear there: the last `console=`
on the command line wins for `/dev/console`, so the service messages and the
banner go to `ttyS0` alone and the panel shows the kernel log, then a shell
prompt. It is a getty and not a bare shell because a shell needs tty1 as its
controlling terminal or Ctrl-C does nothing there, and `cttyhack` cannot
supply it: it reopens whatever `/sys/class/tty/console/active` names last,
which is the serial port.

The LCD data bus uses GPIO33 and GPIO34, which are also the native USB
Serial/JTAG D-/D+ pins, so the loader initializes the display only when the
ESP-IDF secondary console is off. `bootloader/sdkconfig.defaults` selects
`CONFIG_ESP_CONSOLE_SECONDARY_NONE`, so the loader brings the panel up and
`make openocd` has nothing to attach to. Select
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` instead to get JTAG back; the
loader then logs `display disabled: GPIO33/34 reserved for USB Serial/JTAG`
and `/dev/fb0` still registers from the DTB but drives nothing. Flashing and
the Linux console run over the CP2102N bridge either way.

The board's USB Type-A connector is exposed as a Linux high-speed USB host.
An ESP32-S31 PHY driver performs the ESP-IDF clock, reset, UTMI, and host
pull-down sequence, then the standard DWC2 host controller handles the bus.
USB HID and evdev support are built in, so keyboards and mice appear under
`/dev/input`, and the VT layer feeds their key events straight to the
foreground virtual terminal. The controller uses its internal (buffer) DMA,
which GHWCFG2 advertises; descriptor DMA stays off. The boot log states which
mode was chosen (`dwc2 20300000.usb: using internal DMA`).

USB mass storage is deliberately **not** enabled. Selecting `CONFIG_SCSI`,
`CONFIG_BLK_DEV_SD` and `CONFIG_USB_STORAGE` hangs the boot between
`parse_args()` and `setup_log_buf()`, before any of their initcalls could
run, and the hang moves with kernel layout: adding a few `pr_notice()` calls
to `start_kernel()` makes the same configuration boot. It is not an image-size
threshold; padding an otherwise identical kernel past the failing size boots
fine. Root cause unknown.

## Kernel

The kernel is a stock upstream release. Nothing is copied over the extracted
tree: `linux/patches/` is the complete delta, and Buildroot applies it to its
own download through `BR2_LINUX_KERNEL_PATCH`.

The series has two halves. `0000-esp32s31-add-source-files.patch` is
generated by `scripts/mkkernelpatches.py` from the files under `linux/`, which
are all new to the kernel -- the drivers, the device trees, the CLIC
trampoline and the bindings. Edit those files, not the patch, and run
`make kernel-patches`. The numbered patches after it are handwritten and
modify files that already exist upstream; `0001` is the only one that touches
generic architecture code.

The configuration is not in the kernel tree either. It lives in
`br2-external/board/esp32s31/linux.config`, which Buildroot reads directly:
`make kernel-menuconfig` to change it and `make kernel-saveconfig` to write it
back. The DTB is built into the Image, so the loader has one blob to place.

`external/linux` builds nothing. It is the pristine tree the series is written
against, and `make kernel-check` dry-runs every patch on it -- which is also
the test a version bump has to pass. Bumping means changing
`BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE`, moving the submodule to the matching
tag, and fixing whatever `make kernel-check` then reports.

## SD card

The Korvo-1 microSD slot sits on SDMMC slot 0 (dedicated pads GPIO20–25,
routed via IO MUX; the board's unpopulated SPI NAND footprint shares these
pins). The loader powers the slot through the board's active-low enable on
GPIO39, derives the 50 MHz controller clock from the already-running MPLL,
releases the module reset, hands the pads to the SD host, and reports
card-present/not-write-protected through the GPIO matrix constant inputs,
since the slot has no CD/WP contacts.

Linux drives the controller with the stock `dw_mmc` driver (the S31 SDHOST
is a Synopsys DesignWare MSHC). Transfers use the controller's internal DMA:
the host's PIO FIFO port is non-functional in silicon (CPU reads never pop
the FIFO, they return the same latched word, and ESP-IDF never touches that
register either), so IDMAC is the only working data path. Patch
`0003-mmc-dw_mmc-add-esp32s31-support.patch` identifies the SoC integration
and takes the descriptor ring from `dma_alloc_noncoherent()` instead of
`dma_alloc_coherent()`, syncing the whole ring around each ownership
hand-off. The ring is synced as a unit because descriptors are 16 bytes and
several share a cache line.

FAT (VFAT) and ext4 are enabled. The card carries both: an MBR whose first
partition is FAT32 and whose second is the ext4 root. `/etc/init.d/S10sdcard`
mounts the first on `/mnt/sd` when it is present, and never fails the boot
when it is not. Cards can also be mounted manually:

```sh
mount /dev/mmcblk0p1 /mnt/sd
```

A working card enumerates as `/dev/mmcblk0`, but enumeration alone proves
only the command path: verify the data path by comparing a file's
`sha256sum` on the board against the host.

The write path is verified: a 1 MiB random file hashes identically on the
board, on the Mac, and on the board again after reinsertion.

Cards must use MBR. The kernel enables `CONFIG_MSDOS_PARTITION` and no GPT
parsing, so a GPT card enumerates as a bare `mmcblk0` with no partitions at
all. The first partition is FAT32 and as large as you like; the second is the
roughly 192 MiB ext4 root. `make sdpart` lays out a card larger than the
image, keeping most of it as FAT; `make sdwrite` writes the whole card image
instead, which suits a card no bigger than it. Either erases everything.
`make sdroot` then rewrites only the root partition, and is the loop to use
once a card exists.

## Userspace

Buildroot 2026.08 builds the whole RV32 musl userspace, pinned as a submodule
with its configuration in `br2-external/`. It builds the kernel as well, from
a stock release and the patch series; only OpenSBI and the loader keep their
own paths, because both need the Espressif toolchain.

The root password is `korvo-bringup`; the serial and panel gettys log root in
automatically, and Dropbear wants the password. `gdbserver` goes on the card
while its cross-GDB stays in the container.

Four things happen at boot so the board is usable without a console:

- `S01clock` restores the time of the last clean shutdown or NTP sync from
  `/etc/timestamp`, and only ever moves the clock forwards. The board has no
  RTC, so without it every boot starts at the epoch and TLS fails.
- `S05swap` creates and enables a swap file on the root the first time it
  runs, up to 64 MiB and never within 32 MiB of filling the partition. 16 MiB
  of PSRAM is the whole machine; a file rather than a partition works whatever
  layout the card was given.
- `S40wifi` joins the saved network in the background. `wifi <ssid>
  [passphrase]` saves the credentials once the firmware confirms the
  association, so a typo is never stored; `wifi forget` drops them.
- The udhcpc hook runs a one-shot `ntpd` when the year is still implausible,
  then records the result for the next boot.

`e2fsprogs` is on the card because the root is on removable media: the
initramfs mounts it `errors=remount-ro`, and a card that goes read-only is
repaired with `e2fsck -f /dev/mmcblk0p2` from a console.

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
  `make openocd`. These are the LCD data pins, so it is unavailable once the
  loader has initialized the display.
- **USB Type-A host**: This connector is wired to the ESP32-S31 USB 2.0 OTG
  high-speed peripheral and supplies attached devices from the board's
  current-limited 5 V VBUS path. It is independent of the two debug ports.

On macOS, the CP2102N normally appears as `/dev/cu.usbserial-*` and the native
USB Serial/JTAG breakout as `/dev/cu.usbmodem*`.

## Host requirements

- macOS with Homebrew
- ESP-IDF at `~/esp/esp-idf`
- the ESP-IDF `riscv32-esp-elf` toolchain
- `brew install make`
- Apple's `container` CLI, with `container system start` already run

Buildroot does not support running on macOS, so it runs in the Debian image
from `container/Containerfile`, as the invoking user so that files coming back
are owned correctly. Its `output/` and `dl/` stay in the `esp32s31-br` volume;
several gigabytes have no business on virtiofs.

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
the Linux console.

`make build` produces the loader and OpenSBI on the Mac and everything else in
the container: Buildroot downloads the kernel, applies `linux/patches/`,
builds the Image and its flash manifest, then the userspace and the card
images. `make kernel` rebuilds only the kernel. Flashing alone is not enough
to boot: without a provisioned card the initramfs lands in its recovery
shell.

The monitor waits up to 300 seconds (`BOOT_TIMEOUT`) for the boot banner
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
=== ESP32-S31 Linux / Buildroot ===
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
a HID device should add a USB device and an `event*` input node, and its keys
reach the `/dev/tty1` shell on the panel.

`wifi`, the udhcpc hook and the init scripts are installed from
`br2-external/board/esp32s31/rootfs-overlay`. `S99banner` prints the line the
monitor waits for, so that text is a contract with `--success-pattern`.

To join a network and reach the internet, from either console:

```sh
wifi <ssid> [passphrase]
ping 8.8.8.8
```

`wifi` hands the credentials to the firmware through sysfs, waits for the
carrier, then takes a DHCP lease with `udhcpc`. It saves them only once the
association succeeds, and `S40wifi` replays them at every later boot, so this
is a one-time step per network. `wifi off` disconnects, `wifi --saved`
reconnects and `wifi forget` drops the credentials.

## Flash layout

| Offset | Contents |
| ---: | --- |
| `0x00002000` | ESP-IDF second-stage bootloader |
| `0x00008000` | partition table |
| `0x00020000` | ESP32-S31 Linux loader and Wi-Fi firmware |
| `0x00220000` | OpenSBI fw_jump |
| `0x002a0000` | Linux Image |
| `0x00a1fff4` | Linux size and CRC manifest |
| `0x00a20000` | 2 MiB initramfs |

The flash slot no longer holds a userspace. It carries an initramfs of
BusyBox, the musl it links against, and an init that waits for
`/dev/mmcblk0p2`, mounts it and `switch_root`s into the Buildroot rootfs. When
the card is missing or unreadable it drops to a shell there instead, which is
the only reason it survives now that the rootfs is on removable media. Keeping
it also means `CONFIG_CMDLINE_FORCE=y` and `rdinit=/init` never have to change
to move the root filesystem.

Internal SRAM is shared between the two harts. The firmware keeps the ESP-IDF
heap out of `0x2f040000`-`0x2f060000`: the lower half is the kernel's coherent
DMA pool and the upper half carries the Wi-Fi rings. Both are reached without
the data cache, which is what makes them usable from both harts at once.

## Debugging

JTAG and the display are mutually exclusive: both need GPIO33/34. The
checked-in `bootloader/sdkconfig.defaults` selects the panel, so switch it to
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` and rebuild the loader first.
Editing that file is enough: the `bootloader` target discards an `sdkconfig`
older than the defaults, because idf.py otherwise reads the defaults only when
it first creates that file and silently keeps the old configuration.

The breakout is wired white/D- to GPIO33 and green/D+ to GPIO34. Reversed, it
does not enumerate at all -- and neither does it while a loader built for the
panel owns those pins, so confirm the loader logged that it skipped the
display before blaming the wiring. macOS also asks once whether to allow the
accessory.

With the GPIO33/34 native USB breakout connected, start OpenOCD:

```sh
make openocd
```

Then connect the ESP RISC-V GDB to port 3333 using
`build/opensbi.elf`, `build/linux/vmlinux`, or
`build/bootloader/s31-linux-loader.elf` as appropriate.

Boot logs are written under `logs/` and ignored by Git.

## Wi-Fi

The WLAN modem belongs to the firmware on hart 0, which runs the 802.11 side
with ESP-IDF and never touches the flash again once Linux is running: a flash
transaction disables the cache the kernel executes from, so radio bring-up is
ordered ahead of releasing hart 1. NVS is disabled for the same reason, at the
cost of a full RF calibration on every boot.

Linux sees an Ethernet-class `eth0` from `esp32s31-wifi`, which exchanges 802.3
frames with that firmware through fixed-size slot rings and a pair of
cross-core doorbell interrupts. Association is not implemented yet, so the
interface has no carrier.

## Licensing

The repository `LICENSE` (MIT) covers the build system, scripts and rootfs
pieces. Components carry their own licenses in their SPDX headers: the
loader and firmware under `bootloader/` and the OpenSBI platform are
BSD-2-Clause, the kernel drivers and patches under `linux/` are GPL-2.0, and
`shared/esp32s31-wifi-ipc.h` is dual GPL-2.0/BSD-2-Clause because both worlds
compile it.

## Current limitations

- Wi-Fi is an Ethernet-style netdev fed by the hart 0 firmware over shared
  memory, so there is no cfg80211 and no scan: the IPC carries CONNECT and
  DISCONNECT only, and `wifi <ssid> [passphrase]` pushes credentials through
  sysfs;
- Linux is uniprocessor on hart 1, and hart 0 is not available to it;
- the PMP entry OpenSBI installs is a locked global RWX grant, so its domain
  isolation is intentionally unavailable;
- APM/PMS permissions are broad bring-up grants;
- `poweroff` and `halt` park the hart rather than cutting power, because the
  part has no power switch and OpenSBI does not carry the PMU deep-sleep
  sequence;
- USB host carries the HID class only; mass storage and USB networking are
  not enabled;
- coherent DMA allocations all come from one 64 KiB SRAM pool, so a driver
  that wants a large coherent buffer will fail to allocate;
- most board peripherals other than the panel, SD slot, USB host and WLAN
  modem are not enabled yet;
- there is no audio. The kernel has no `SND_SOC`, no I2C and no ESP32-S31
  ASoC driver, so the Korvo-1's codec is unreachable; `mpg123` and
  `alsa-utils` were dropped from the card rather than ship players with
  nothing to play through;
- swap is a file on the SD card. It is what makes 16 MiB usable, and it wears
  the card: `swapoff /swapfile && rm /swapfile` if that matters more than the
  memory;
- `strace` is absent. Buildroot excludes it on RV32 in
  `package/strace/Config.in`, and upstream strace has no `src/linux/riscv32`
  on any branch, so this needs a port rather than a configuration change.
  RV32 and RV64 share the `asm-generic` syscall table, so the port is mostly
  a matter of adapting `src/linux/riscv64` to 32-bit registers and the
  `__NR3264` name mapping. `gdbserver` is the debugger on the board
  meanwhile.
