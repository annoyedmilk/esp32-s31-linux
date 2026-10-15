# ESP32-S31 Linux

Linux 7.2 and OpenSBI 1.9 on the ESP32-S31-Korvo-1 board. Linux uses the
16 MiB octal PSRAM as its memory.

```text
ESP ROM -> ESP-IDF 2nd stage -> loader -> OpenSBI -> Linux -> initramfs -> Buildroot rootfs (SD)
```

![fastfetch on the ESP32-S31-Korvo-1](docs/fastfetch.svg)

## About

The ESP32-S31 is a microcontroller: two RV32 harts at 320 MHz, 512 KiB of
SRAM, Wi-Fi 6 and Bluetooth. It was not made for Linux. This project runs a
stock Linux 7.2 kernel with a small patch set on it, with a real userspace:
a shell on the LCD, Wi-Fi with `wpa_supplicant`, Bluetooth with BlueZ, SSH,
USB keyboards and sticks, `vim`, `gdb`, `strace`, MicroPython and a text-mode web browser
(`links`).

You can use it in two ways:

- **To try it.** Flash a release, write the card image, and log in. It
  takes about ten minutes.
- **To learn from it.** The SoC has no PLIC, no Zicbom and no coherent DMA.
  Each of these has a solution here that you can read in a few hundred
  lines. [docs/internals.md](docs/internals.md)
  tells the full story for kernel developers.

## Try it

1. Get the files of the newest release from the
   [releases page](https://github.com/annoyedmilk/esp32-s31-linux/releases).
2. Flash: `pip install "esptool>=5.4"`, connect the UART Type-C port, then
   `esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin`.
3. Write `*-sdcard.img.xz` to a microSD card (for example with
   balenaEtcher), and put the card into the board.
4. Open the console on the UART port at 115200 baud. Push reset. After
   about 30 seconds you are root.
5. Connect to Wi-Fi (see [Wi-Fi](#wi-fi)), then try `ssh`, `links`,
   `micropython` or `iperf3`.

The root password is `korvo-bringup`. Change it before you connect the board
to a network that you do not control.

## Releases

A tag `v*` starts `.github/workflows/release.yml`. It builds everything on
a Linux runner (`scripts/ci-build.sh`), packages the files
(`scripts/package-release.sh`) and publishes a GitHub release. A tag with
a `-` (for example `v0.2.0-rc1`) makes a pre-release. Usual commits do not
start a build. To test the workflow without a release, run it by hand from
the Actions tab: the files are then workflow artifacts only.

The ESP-IDF commit is pinned in `bootloader/esp-idf.version`. Use the same
commit for a local build.

## What works

- UART console (`ttyS0`) on the CP2102N bridge.
- 800x480 LCD as `/dev/fb0` with `fbcon`, and a login on `tty1`.
- USB host (DWC2): keyboards, mice and mass storage.
- microSD (`dw_mmc` with internal DMA): FAT on p1, ext4 root on p2.
- Wi-Fi as `wlan0` (cfg80211 full-MAC) through the ESP-Hosted firmware on
  hart 0: station with `wpa_supplicant` (WPA2 and WPA3-SAE), access point
  with `hostapd`. iperf3 TCP gives approximately 35 to 38 Mbit/s in each
  direction.
- Bluetooth as `hci0` through the same firmware: BR/EDR and LE, with
  `bluetoothd` and `bluetoothctl`.
- IEEE 802.15.4 as an OpenThread radio co-processor: the Spinel stream is
  `/dev/esp_rcp0`, for a Thread host such as `ot-daemon`. The image has no
  Thread host.
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
  resident as the radio firmware. Linux runs on hart 1 only.
- **Loader.** The loader checks the size and CRC of the kernel. It copies the
  kernel to `0x50000000`, the initramfs to `0x50800000` and OpenSBI to
  `0x50F00000`. It starts the LCD, the SD slot and Wi-Fi. Then it releases
  hart 1 into OpenSBI.
- **Interrupts.** OpenSBI gives the CLIC inputs to S-mode. If an input is not
  in S-mode, Linux cannot see it. The S-mode trampoline must use `sret` and
  restore `scause.spil`. `mret` locks `mintstatus.SIL` and masks all
  supervisor interrupts.
- **Cache.** PSRAM is write-back. The panel DMA reads the frame buffer from
  PSRAM without the cache, so OpenSBI makes the frame buffer range uncached
  with PMA entries. No bus master is coherent with the data cache, and the
  hart has no Zicbom. DMA uses the cache sync engine, which OpenSBI programs
  for Linux through a vendor SBI extension
  (`drivers/cache/esp32s31-cache.c`). The I-cache refills from PSRAM, not
  from the D-cache, so Linux writes the D-cache back before each `fence.i`.
  Coherent DMA memory comes from a 64 KiB pool in SRAM at `0x2F040000`.
- **Shared SRAM.** `0x2F040000`-`0x2F05D000` is outside the ESP-IDF heap:
  the DMA pool, then the ESP-Hosted rings at `0x2F050000`
  (`shared/esp32s31-hosted-shm.h`).
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
`bootloader/sdkconfig.defaults`). For JTAG, set
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` instead.

## Host requirements

- macOS with Homebrew and GNU make (`brew install make`).
- ESP-IDF at `~/esp/esp-idf`, at the commit in `bootloader/esp-idf.version`,
  with the `riscv32-esp-elf` toolchain and the ESP-Hosted patches
  (`make bootloader` gives the command).
- Apple `container` CLI. Run `container system start` first.

On Linux, `scripts/ci-build.sh` builds everything without a container.

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

Without a prepared SD card, the initramfs starts a recovery shell on the UART
and one on the LCD.

## SD card

The card is MBR. p1 is FAT32 and is mounted on `/mnt/sd`. p2 is the ext4 root
(192 MiB). All card targets erase data. They ask for confirmation and `sudo`.

| Command | Use |
| --- | --- |
| `make sdpart SD_DISK=/dev/diskN` | Partition a large card. p1 gets `SD_DATA_SIZE` (28G). |
| `make sdwrite SD_DISK=/dev/diskN` | Write the full card image. Use for a small card. |
| `make sdroot SD_DISK=/dev/diskN` | Write only the root partition. Use for updates. |

If the root goes read-only, run `e2fsck -f /dev/mmcblk0p2` from the console.

When the board is reset with p1 mounted, the FAT keeps a dirty flag. Linux
does not clear it, also not at a clean shutdown, and warns at each mount.
`S10sdcard` runs `fsck.fat -a` on a dirty p1 before it mounts it.
`fsck.fat` needs approximately 8 bytes of memory for each cluster. That is
less than 1 MB for the 64 MiB p1 of the card image, and more than the board
has for a 28 GiB p1. `S10sdcard` skips such a large p1: check it on the host.

## Userspace

The root password is `korvo-bringup`. The serial and panel consoles log in
automatically. Dropbear SSH asks for the password.

Boot scripts in `br2-external/board/esp32s31/rootfs-overlay`:

- `S01clock` sets the clock from `/etc/timestamp`. The board has no RTC.
- `S05swap` starts 6 MiB of LZ4 swap in RAM (`/dev/zram0`, priority 100),
  then a swap file of max. 64 MiB on the root (priority 10).
- `S10sdcard` mounts p1 on `/mnt/sd`.
- `S40wifi` starts `wpa_supplicant` on `wlan0`, and `S40bluetoothd` starts
  BlueZ for `hci0`.
- The udhcpc hook sets the clock with NTP on each new lease.
- `S95autostart` runs `/mnt/sd/autostart.sh`, if that file exists.
- `S99banner` prints the line that `make monitor` waits for.

`login` and SSH show `/etc/motd`, which lists the first commands.

### Tools

| Use | Commands |
| --- | --- |
| System | `htop`, `free`, `df -h`, `dmesg`, `file`, `strace`, `gdbserver` |
| Network | `ssh`, `scp`, `curl`, `links`, `iperf3`, `tcpdump`, `nslookup` |
| Code | `micropython`, `lua`, `vim` |
| Hardware | `gpioinfo`, `gpioget`, `gpioset` (libgpiod 2), `lsusb` |

The shell is bash, with a history that stays after a reset.

### Files and scripts

- **Copy files.** From a computer, `scp` and `sftp` work with the root
  password: `scp file root@<address>:/mnt/sd/`.
- **Web server.** `httpd -f -p 8080 -h /mnt/sd` gives the files of the card
  to a browser, at `http://<address>:8080/<file name>`. It shows no list of
  the files: a directory needs an `index.html`. Stop it with Ctrl-C.
- **Start at boot.** The board runs `/mnt/sd/autostart.sh` at each boot, as
  root. The file is on the FAT partition, so a computer can also write it,
  and it stays after `make sdroot`.
- **Examples** in `/usr/share/examples`: `led.py` sets the RGB LED from
  MicroPython, `gpio.sh` reads or sets a pin, `www/` is a web page with a CGI
  script for the LED, and `autostart.sh` is a start script.

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

The ESP-Hosted firmware on hart 0 runs the 802.11 MAC. The ESP-Hosted host
driver (`esp32_shm`) is built into the kernel. `wpa_supplicant` runs the
authentication and the key handshakes. `hostapd` with the nl80211 driver
starts an access point. Stop `S40wifi` first: `wlan0` is a station or an
access point, not the two at the same time.

## Kernel

Buildroot builds a stock kernel release (`BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE`
in `br2-external/configs/esp32s31_defconfig`) with `linux/patches/`.

- `0000-esp32s31-add-source-files.patch` is generated from the new files
  under `linux/`. Edit those files, not the patch. Each
  `make kernel`, `make rootfs` and `make kernel-patches` makes the patch
  again, and an edit of the patch is lost.
- `0001` and higher are manual patches to existing kernel files.
- The ESP-Hosted host driver comes from `external/esp-hosted-linux`, with the
  patches in `br2-external/package/esp-hosted-linux/`. A Buildroot linux
  extension (`br2-external/linux/`) copies it into the kernel tree.
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
| `0x020000` | Loader and radio firmware |
| `0x220000` | OpenSBI `fw_jump` |
| `0x2A0000` | Linux Image (with built-in DTB) |
| `0xA1FFF4` | Linux size and CRC manifest |
| `0xA20000` | 2 MiB initramfs |

The initramfs holds BusyBox and an `init` that mounts `/dev/mmcblk0p2` and
does `switch_root`. If the card is not there, it starts a shell on the UART
and one on the LCD.

## Debugging

1. Set the loader to JTAG (see above). The LCD then stays dark.
2. Run `make flash`, then `make openocd`.
3. Connect `riscv32-esp-elf-gdb` to port 3333. Use `build/opensbi.elf`,
   `build/bootloader/s31-linux-loader.elf` or `build/vmlinux`
   (`make kernel-vmlinux`).

## Limits

- Linux is uniprocessor on hart 1.
- The hardware FPU is not used: the hart has F but not D, and Linux supports
  an FPU only with D. Floating point works, because userspace is built
  soft-float (ilp32), but it is slower.
- OpenSBI protects its memory and the TEE/APM registers from S-mode with
  PMP, and its memory from DMA with HP_APM. The radio firmware on hart 0
  runs in M-mode, so nothing protects OpenSBI from hart 0.
- `poweroff` stops the hart. It does not remove power.
- The coherent DMA pool is 64 KiB.
- There is no I2C, audio or camera driver.
- The firmware turns the Wi-Fi/802.15.4 coexistence scheme off, because
  it halves the Wi-Fi throughput also when no Thread host runs. A Thread
  network on the RCP can lose frames while Wi-Fi is busy.
- With the three radios, the firmware heap has approximately 64 KiB free
  when idle and 25 KiB during iperf3. Bluetooth connections and Thread
  traffic need more, which is not measured.
- BR/EDR is tested up to the inquiry: no discoverable device was near.
- WPA2/WPA3 Enterprise is not tested.

## License

`LICENSE` (MIT) applies to the build system, scripts and rootfs files. Other
files have SPDX headers: `bootloader/` and the OpenSBI platform are
BSD-2-Clause, `bootloader/components/esp_hosted/` is Apache-2.0, `linux/`
and `br2-external/package/esp-hosted-linux/` are GPL-2.0,
`shared/esp32s31-hosted-shm.h` is GPL-2.0 or Apache-2.0.
