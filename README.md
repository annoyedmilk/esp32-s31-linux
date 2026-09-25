# ESP32-S31 Linux

Linux 7.2 and OpenSBI 1.9 on the ESP32-S31 Korvo-1 board. Linux uses the
16 MiB octal PSRAM as its memory.

```text
ESP ROM -> ESP-IDF 2nd stage -> loader -> OpenSBI -> Linux -> initramfs -> Buildroot rootfs (SD)
```

## What works

- UART console (`ttyS0`) on the CP2102N bridge.
- 800x480 LCD as `/dev/fb0` with `fbcon`, and a login on `tty1`.
- USB host (DWC2): keyboards, mice and mass storage.
- microSD (`dw_mmc` with internal DMA): FAT on p1, ext4 root on p2.
- Wi-Fi as `wlan0` (cfg80211 full-MAC) with `wpa_supplicant`, through the
  firmware on hart 0.
- Hardware RNG, GPIO, `reboot` and `poweroff`.

## Tested hardware

Espressif makes new chip revisions, and a different revision can change
the behavior. This port is tested only on this board:

| Item | Value |
| --- | --- |
| Board | ESP32-S31-Korvo-1 |
| Chip | ESP32-S31, revision v0.0 (wafer major 0, minor 0) |
| Package version (eFuse) | 0 |
| eFuse block version | 0.0 |
| PSRAM (eFuse vendor 1, capacity 1) | 16 MiB octal |
| Flash | 16 MB NOR (JEDEC manufacturer 0x46, device 0x4018) |
| Crystal | 40 MHz |

To read these values from your board:

```sh
python -m esptool --chip esp32s31 -p PORT chip-id
python -m esptool --chip esp32s31 -p PORT flash-id
python -m espefuse --chip esp32s31 -p PORT summary
```

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
- **Cache.** PSRAM is write-back. With the LCD, the Makefile selects
  write-through, because the panel DMA reads PSRAM without the cache. No bus
  master is coherent with the data cache, and the hart has no Zicbom. DMA
  uses the cache sync engine, which OpenSBI programs for Linux through a
  vendor SBI extension (`drivers/cache/esp32s31-cache.c`). The I-cache
  refills from PSRAM, not from the D-cache, so Linux writes the D-cache back
  before each `fence.i`. Coherent DMA memory comes from a 64 KiB pool in SRAM
  at `0x2F040000`.
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
default is JTAG (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` in
`bootloader/sdkconfig.defaults`). For the LCD, set
`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` instead.

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
- `S40wifi` starts `wpa_supplicant` on `wlan0`.
- The udhcpc hook sets the clock with NTP on each new lease.
- `S99banner` prints the line that `make monitor` waits for.

## Wi-Fi

`wlan0` works with the standard `wpa_supplicant`. `S40wifi` starts it with
`/etc/wpa_supplicant.conf`, or with `wpa_supplicant.conf` on p1 when that
file exists. `make sdroot` does not delete p1, and a board without a console
can get its network from that file. At each connection, the `wpa_cli` action
script starts or renews the DHCP lease.

```sh
wpa_passphrase "<ssid>" "<passphrase>" >> /etc/wpa_supplicant.conf
/etc/init.d/S40wifi restart
wpa_cli status
wpa_cli scan; wpa_cli scan_results
```

For WPA3 and WPA2/WPA3 networks, the network block needs
`key_mgmt=WPA-PSK SAE`, `ieee80211w=1` and the passphrase as `psk="..."`.
The hex PSK from `wpa_passphrase` is not sufficient for SAE.

The firmware on hart 0 runs 802.11 and the key handshakes.
`wpa_supplicant` gives it the key through the nl80211 4-way handshake
offload (the PMK) or the SAE offload (the password). The firmware stops
after 3 attempts that do not associate and reports the failure.

## Kernel

Buildroot builds a stock kernel release (`BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE`
in `br2-external/configs/esp32s31_defconfig`) with `linux/patches/`.

- `0000-esp32s31-add-source-files.patch` is generated from the new files
  under `linux/`. Edit those files, then run `make kernel-patches`.
- `0001` and higher are manual patches to existing kernel files.
- The kernel config is `br2-external/board/esp32s31/linux.config`. Use
  `make kernel-menuconfig` and `make kernel-saveconfig`.
- `make kernel-check` applies the series at zero fuzz with GNU patch.
- `make kernel` and `make rootfs` find a changed series or config. Then
  they extract and patch the kernel again. `make kernel-clean` does this by
  hand.

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

1. Make sure that the loader uses JTAG (the default, see above).
2. Run `make flash`, then `make openocd`.
3. Connect `riscv32-esp-elf-gdb` to port 3333. Use `build/opensbi.elf`,
   `build/bootloader/s31-linux-loader.elf` or `build/vmlinux`
   (`make kernel-vmlinux`).

## Limits

- Linux is uniprocessor on hart 1.
- No FPU: the hart has F but not D, and Linux RISC-V needs D. Userspace is
  soft-float (ilp32).
- ESP-Hosted cannot work: Espressif does not supply the FullMAC hooks for the
  ESP32-S31 Wi-Fi libraries.
- OpenSBI sets one locked RWX PMP entry. There is no domain isolation. APM
  permissions are open.
- `poweroff` stops the hart. It does not remove power.
- The coherent DMA pool is 64 KiB.
- No audio, I2C or USB networking yet.

## License

`LICENSE` (MIT) applies to the build system, scripts and rootfs files. Other
files have SPDX headers: `bootloader/` and the OpenSBI platform are
BSD-2-Clause, `linux/` is GPL-2.0, `shared/esp32s31-wifi-ipc.h` is
GPL-2.0 or BSD-2-Clause.
