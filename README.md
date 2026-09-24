# ESP32-S31 Linux

Linux 7.1 and OpenSBI 1.9 on the ESP32-S31 Korvo-1 board. Linux uses the
16 MiB octal PSRAM as its memory.

```text
ESP ROM -> ESP-IDF 2nd stage -> loader -> OpenSBI -> Linux -> initramfs -> Buildroot rootfs (SD)
```

## What works

- UART console (`ttyS0`) on the CP2102N bridge.
- 800x480 LCD as `/dev/fb0` with `fbcon`, and a login on `tty1`.
- USB host (DWC2): keyboards, mice and mass storage.
- microSD (`dw_mmc` with internal DMA): FAT on p1, ext4 root on p2.
- Wi-Fi as `wlan0` (cfg80211 full-MAC), through the firmware on hart 0.
- Hardware RNG, GPIO, `reboot` and `poweroff`.

## Design

- **Harts.** Hart 0 stays in M-mode. It runs the ESP-IDF loader, which stays
  resident as the Wi-Fi firmware. Linux runs on hart 1 only.
- **Loader.** The loader checks the size and CRC of the kernel. It copies the
  kernel to `0x50000000`, the initramfs to `0x50800000` and OpenSBI to
  `0x50E00000`. It starts the LCD, the SD slot and Wi-Fi. Then it releases
  hart 1 into OpenSBI.
- **Interrupts.** OpenSBI gives the CLIC inputs to S-mode. If an input is not
  in S-mode, Linux cannot see it. The S-mode trampoline must use `sret` and
  restore `scause.spil`. `mret` locks `mintstatus.SIL` and masks all
  supervisor interrupts.
- **Cache.** PSRAM is write-through. No bus master is coherent with the data
  cache, and the hart has no Zicbom. DMA uses the cache sync engine
  (`drivers/cache/esp32s31-cache.c`). Coherent DMA memory comes from a 64 KiB
  pool in SRAM at `0x2F040000`.
- **Shared SRAM.** `0x2F040000`-`0x2F060000` is outside the ESP-IDF heap. The
  lower half is the DMA pool. The upper half holds the Wi-Fi rings
  (`shared/esp32s31-wifi-ipc.h`).
- **Display.** The frame buffer is at `0x50F40000`. The loader DMA continues
  to scan it out after the handoff. Linux uses it as a `simple-framebuffer`.

## Hardware connections

| Connector | Use |
| --- | --- |
| Power Type-C | Power only. |
| UART Type-C | CP2102N to UART0: flash and Linux console (`/dev/cu.usbserial-*`). |
| USB Type-A | USB 2.0 host for Linux. |
| GPIO33/34 breakout | Native USB Serial/JTAG (`/dev/cu.usbmodem*`): D- white to GPIO33, D+ green to GPIO34, GND black. Do not connect 5 V. |

GPIO33/34 are also LCD data pins. You can use the LCD or JTAG, not both. The
default is the LCD (`CONFIG_ESP_CONSOLE_SECONDARY_NONE` in
`bootloader/sdkconfig.defaults`).

## Host requirements

- macOS with Homebrew and GNU make (`brew install make`).
- ESP-IDF `master` at `~/esp/esp-idf`, with the `riscv32-esp-elf` toolchain.
- Apple `container` CLI. Run `container system start` first.

Buildroot does not run on macOS. It runs in the Debian image from
`container/Containerfile`. Its `output/` and `dl/` are in the `esp32s31-br`
volume.

```sh
git submodule update --init --recursive
make container-image
make check
make ports
```

## Build, flash and monitor

```sh
make build
make flash   FLASH_PORT=/dev/cu.usbserial-XXXX
make monitor SERIAL_PORT=/dev/cu.usbserial-XXXX
```

- `make build` builds the loader and OpenSBI on the Mac. Buildroot builds the
  kernel, the rootfs and the card images in the container.
- `make monitor` resets the board and waits for
  `=== ESP32-S31 Linux / Buildroot ===`. Then it stays open as a terminal.
  Push `Ctrl-]` to exit. It writes a log to `logs/`.
- Set `RESET_PORT` if the RTS line of `SERIAL_PORT` cannot reset the board.
- `make help` shows all targets.

Without a prepared SD card, the initramfs starts a recovery shell.

## SD card

The card is MBR. p1 is FAT32 and is mounted on `/mnt/sd`. p2 is the ext4 root
(192 MiB). All card targets erase data. They ask for confirmation and `sudo`.

| Command | Use |
| --- | --- |
| `make sdpart SD_DISK=/dev/diskN` | Partition a large card. p1 gets `SD_DATA_SIZE` (28G). |
| `make sdwrite SD_DISK=/dev/diskN` | Write the full card image. Use for a small card. |
| `make sdroot SD_DISK=/dev/diskN` | Write only the root partition. Use for updates. |

If the root goes read-only, run `e2fsck -f /dev/mmcblk0p2` from the console.

## Userspace

The root password is `korvo-bringup`. The serial and panel consoles log in
automatically. Dropbear SSH asks for the password.

Boot scripts in `br2-external/board/esp32s31/rootfs-overlay`:

- `S01clock` sets the clock from `/etc/timestamp`. The board has no RTC.
- `S05swap` makes a swap file of max. 64 MiB on the root.
- `S10sdcard` mounts p1 on `/mnt/sd`.
- `S40wifi` connects to the saved network.
- The udhcpc hook sets the clock with NTP on each new lease.
- `S99banner` prints the line that `make monitor` waits for.

## Wi-Fi

```sh
wifi <ssid> [passphrase]    # connect, get a lease, save on success
wifi scan
wifi off
wifi --saved
wifi forget
```

The credentials are in `/etc/wifi.conf`. `make sdroot` replaces this file. A
`wifi.conf` on p1 has priority and stays. Use it to set up a board without a
console.

The firmware on hart 0 runs 802.11 and its own supplicant. nl80211 cannot
send a passphrase, so the `wifi` script writes it to the `psk` sysfs
attribute. Then it connects through cfg80211.

## Kernel

Buildroot builds a stock kernel release (`BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE`
in `br2-external/configs/esp32s31_defconfig`) with `linux/patches/`.

- `0000-esp32s31-add-source-files.patch` is generated from the new files
  under `linux/`. Edit those files, then run `make kernel-patches`.
- `0001` and higher are manual patches to existing kernel files.
- The kernel config is `br2-external/board/esp32s31/linux.config`. Use
  `make kernel-menuconfig` and `make kernel-saveconfig`.
- `make kernel-check` applies the series at zero fuzz with GNU patch.
- After a patch change, run `make kernel-clean`, then `make kernel`.

## Flash layout

| Offset | Contents |
| ---: | --- |
| `0x002000` | ESP-IDF second-stage bootloader |
| `0x008000` | Partition table |
| `0x020000` | Loader and Wi-Fi firmware |
| `0x220000` | OpenSBI `fw_jump` |
| `0x2A0000` | Linux Image (with built-in DTB) |
| `0xA1FFF4` | Linux size and CRC manifest |
| `0xA20000` | 2 MiB initramfs |

The initramfs holds BusyBox and an `init` that mounts `/dev/mmcblk0p2` and
does `switch_root`. If the card is not there, it starts a shell.

## Debugging

1. Set `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` in
   `bootloader/sdkconfig.defaults`. The LCD stays dark.
2. Run `make flash`, then `make openocd`.
3. Connect `riscv32-esp-elf-gdb` to port 3333. Use `build/opensbi.elf`,
   `build/bootloader/s31-linux-loader.elf` or `build/vmlinux`
   (`make kernel-vmlinux`).

## Limits

- Linux is uniprocessor on hart 1.
- The passphrase goes through sysfs, not `wpa_supplicant`.
- ESP-Hosted cannot work: Espressif does not supply the FullMAC hooks for the
  ESP32-S31 Wi-Fi libraries.
- OpenSBI sets one locked RWX PMP entry. There is no domain isolation. APM
  permissions are open.
- `poweroff` stops the hart. It does not remove power.
- The coherent DMA pool is 64 KiB.
- No audio, I2C or USB networking yet.
- No `strace`: Buildroot and upstream strace do not support RV32.

## License

`LICENSE` (MIT) applies to the build system, scripts and rootfs files. Other
files have SPDX headers: `bootloader/` and the OpenSBI platform are
BSD-2-Clause, `linux/` is GPL-2.0, `shared/esp32s31-wifi-ipc.h` is
GPL-2.0 or BSD-2-Clause.
