Linux 7.2 on the ESP32-S31, ready to flash: for the ESP32-S31-Korvo-1, and
for a bare ESP32-S31 module with 16 MiB PSRAM and UART only.

## Files

| File | Use |
| --- | --- |
| `esp32s31-linux-*-korvo1-flash.zip` | Korvo-1 flash images. `flash.bin` goes to offset 0. `FLASH.txt` tells how. |
| `esp32s31-linux-*-korvo1-sdcard.img.xz` | Korvo-1 microSD card image: FAT for your files and the ext4 root. |
| `esp32s31-linux-*-esp32s31-flash.zip` | Flash images for a bare module. The full system is in the flash. |
| `SHA256SUMS` | Checksums of the files. |

## Quick start

1. `pip install "esptool==5.5.0"` (the tested version)
2. Connect the UART port of the board (the UART Type-C port on the Korvo-1).
3. `esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin`
4. Korvo-1 only: write the card image to a microSD card (all data on the
   card is lost), for example with balenaEtcher. Put the card into the
   board.
5. Open the console on the same port at 115200 baud, and push the reset
   button. The board logs in as root.
6. Wi-Fi: `wpa_passphrase "<ssid>" "<passphrase>" >> /etc/wpa_supplicant.conf`,
   then `/etc/init.d/S40wifi restart`. On the bare module, use
   `key_mgmt=WPA-PSK` (it has no WPA3). The file stays after a reboot.

On the Korvo-1, the LCD shows the Tux logo, the kernel warnings and a
login. A USB keyboard works on it.

## New since v0.4.0

- The bare build keeps its root in the flash as OpenWrt does: a read-only
  squashfs with a writable UBIFS overlay. All changes stay, and
  `persist save` is gone. `flash_erase /dev/mtd1 0 0` and a reboot go back
  to the image.
- Linux reads the flash through the cache, and it runs on while the
  firmware programs or erases the flash. Writes in the overlay no longer
  stop Linux, and they are faster.
- SSH takes root only with a key. Put your public key into
  `/root/.ssh/authorized_keys` from the console.
- The Korvo-1 kernel mounts the root on the card directly, without an
  initramfs. The boot is faster, and `flash.bin` is smaller. Without a
  card, the kernel waits for one: there is no recovery shell.
- The regulatory database is in the kernel, so the country of the access
  point applies from the first connection.
- Swap goes to zram only. A swap file in `/etc/fstab` still works.
- DHCP uses the udhcpc script of Buildroot: search domains and static
  routes of the lease work.
- For your own builds: files in `local-overlay/` go into the root, for
  example an SSH key and the Wi-Fi network.
- The esptool version that flashes the board is pinned
  (`bootloader/esptool.version`).
- The kernel keeps a larger free reserve, so that swap to zram does not
  fail when memory is low.

The next release needs the new `flash.bin`: the partition tables of both
builds changed. On the bare build, the first boot formats `data`.

## New in v0.4.0

- Two builds: `korvo1` for the ESP32-S31-Korvo-1, and `esp32s31` for a bare
  module with UART only. The bare build has its full system in the flash,
  gives Linux 12.6 MiB of RAM, and boots in approximately 6 seconds.
- `persist save` on the bare build keeps files such as the Wi-Fi network
  and the SSH host keys in a 64 KiB flash partition.
- Wi-Fi is fast again: iperf3 TCP gives approximately 43 to 46 Mbit/s TX
  and 37 to 39 Mbit/s RX on the Korvo-1, and 46 Mbit/s in each direction on
  the bare build (v0.3.1: 35 to 38). The IEEE 802.15.4 RCP is removed: its
  SRAM goes to the Wi-Fi buffers.
- The default `wpa_supplicant.conf` has `pmf=1`. Without protected
  management frames, a WPA2/WPA3 router gave half of the TX throughput.
- `poweroff` puts the chip into deep sleep. Before, it only stopped the
  CPU of Linux.
- The radio firmware on hart 0 can no longer write the memory of Linux: it
  locks itself out of PSRAM with PMP.
- `make sdpart` formats the FAT partition with 64 KiB clusters, so the boot
  can check a dirty 28 GiB partition (3.3 MiB, not 13 MiB).
- A Logitech receiver and other USB devices with vendor HID reports no
  longer give "device has no listeners": the kernel has `hidraw`.
- `wpa_supplicant` logs to syslog, not to the console.
- Linux 7.2.11 and the current ESP-Hosted.
- The Buildroot defconfigs are now `korvo1_defconfig` and
  `esp32s31_defconfig`. Before, `esp32s31_defconfig` was the Korvo-1 build.

v0.4.0 needs the new `flash.bin`: the partition table changed. On the
Korvo-1, also write the new card image.

## New in v0.3.1

- Bluetooth: `hci0` with BR/EDR and LE through the ESP-Hosted firmware on
  hart 0. `bluetoothd`, `bluetoothctl`, `hciconfig` and `hcitool` are on
  the card.
- IEEE 802.15.4: an OpenThread radio co-processor. Its Spinel stream is
  `/dev/esp_rcp0`, for a Thread host such as `ot-daemon` (not on the card).
- The kernel log no longer shows a Wi-Fi error every 300 s. The OBSS scan
  of `wpa_supplicant` now runs.
- The ESP-Hosted driver is built into the kernel. The kernel has no module
  support.
- `/bin/sh` is BusyBox ash, which needs less memory for the boot scripts.
  The login shell stays bash.
- Swap goes first to 6 MiB of LZ4-compressed RAM, then to the swap file on
  the card.
- The userspace uses the Zba, Zbb and Zbs instructions of the CPU.
- A boot after a reset checks a dirty FAT partition, so the kernel does
  not warn about it.
- The boot is quieter and faster: no PSRAM test, and the consoles show
  only warnings and errors.
- OpenSBI 1.9 and the current ESP-Hosted. OpenSBI moved to `0x50F00000`.

Wi-Fi gives approximately 35 to 38 Mbit/s in each direction (v0.3.0: 46
TX, 41 to 43 RX): the radios take SRAM from the Wi-Fi buffers. The
firmware turns the Wi-Fi/802.15.4 coexistence off, so a Thread network can
lose frames while Wi-Fi is busy.

v0.3.1 needs the new `flash.bin` and the new card image together. A v0.3.0
card loads the Wi-Fi driver as a module, which v0.3.1 does not have.

## New in v0.3.0

- Wi-Fi is now ESP-Hosted from Espressif. The ESP-Hosted firmware runs on
  hart 0, and Linux loads its host driver as the module `esp32_shm`. The
  two sides use a shared-memory transport. The own Wi-Fi driver and its
  firmware of v0.2.x are gone.
- `wpa_supplicant` on Linux runs the authentication and the key handshakes
  (WPA2 and WPA3-SAE). `hostapd` is on the card and starts an access point.
- Wi-Fi is faster: approximately 46 Mbit/s TX and 41 to 43 Mbit/s RX with
  iperf3 TCP (v0.2.2: 39 and 46 Mbit/s).
- Linux 7.2.10 and ESP-IDF v6.1 (before: Linux 7.2.9 and a development
  version of ESP-IDF).
- A program starts faster the first time after a file change: Linux syncs
  the caches only for the new code page, not for the full caches.
- `fastfetch` shows the system, the kernel, the memory and the network.

v0.3.0 needs the new `flash.bin` and the new card image together. A v0.2.x
card does not have the Wi-Fi module, and the v0.3.0 card does not work
with a v0.2.x `flash.bin`.

## Fixed in v0.2.2

- A program that writes machine code and then runs it could run the old
  code: for example a JIT, or a breakpoint that `gdbserver` sets. The
  `fence.i` instruction does not clear the instruction cache of this chip,
  so the old code stayed in it for up to one timer tick. Now OpenSBI clears
  the instruction cache when Linux makes new code visible. v0.1.0 to v0.2.1
  have this fault.

From v0.2.0 or v0.2.1, flash `flash.bin` only. The card image has no change.

## Fixed in v0.2.1

- The board did not start in about 2% of the boots. The log stopped after
  `hart 0 resident`, and only the reset button helped. v0.1.0 and v0.2.0
  have this fault. The cause was the sequence in which OpenSBI sets the
  memory permissions of the chip.
- The start of the Linux hart wrote into memory of the Wi-Fi firmware. That
  memory is now reserved.

From v0.2.0, flash `flash.bin` only. The card image has no change.

## New since v0.1.0

- Linux 7.2.9 and a newer ESP-IDF, with a fix in the PHY library.
- Linux is 2 to 10 times faster with the LCD. The frame buffer is now
  uncached, so the other memory can use the cache in write-back mode. The
  boot to the login is also faster.
- Linux has 264 KiB more memory. Kernel parts that nothing on the board
  uses are off.
- More tools: `htop`, `curl`, `file`, and the BusyBox web server (`httpd`).
  `scp` and `sftp` from a computer now work, because the image has an SFTP
  server.
- The GPIO tools work. They are now from libgpiod 2 (`gpioget -c gpiochip0
  PIN`), because the kernel has only version 2 of the GPIO interface.
- A login message lists the first commands. The shell has a prompt with the
  directory, and its history stays after a reset.
- The board runs `/mnt/sd/autostart.sh` at each boot, if that file exists.
- Examples in `/usr/share/examples`: the RGB LED from MicroPython, a GPIO
  script, and a web page that sets the LED.
- Without a card, there is a recovery shell on the LCD also, for a USB
  keyboard. Ctrl-C works in it.
- `fsck.fat` is in the image. It clears the "not properly unmounted" flag
  of the FAT partition. The README tells how.
- `docs/internals.md` describes the two ways to get uncached access to
  PSRAM.

The recovery shell on the LCD and `fsck.fat` are from Platima.

To update from v0.1.0, flash `flash.bin` and write the new card image. The
card image erases the card, so save your files from the FAT partition first.

## Known limits

- The Korvo-1 with chip revision v0.0 is the only tested hardware. The
  bare build is tested on the Korvo-1.
- One hart runs Linux. The other runs the Wi-Fi firmware.
- Floating point works in software; the hardware FPU is not used.
- No I2C, audio or camera driver.
- `poweroff` puts the chip into deep sleep. It does not remove power.
- With WPA3-SAE, the first connection can fail, and the second one works.
- The bare build has WPA2 only and no BlueZ.

See the README for the full list, and `docs/internals.md` for the details.
