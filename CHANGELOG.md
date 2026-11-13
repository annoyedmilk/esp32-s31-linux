# Changelog

Each version, newest first. The update notes come first in each version.

## v1.0.0

- Update: write the new `flash.bin` on all boards. On the Korvo-1 and the
  Function-CoreBoard-1, also write the disk image of the same release: the
  kernel loads its modules from the disk.
- Update: the start scripts read `/etc/default/*` without a fallback. A
  deleted file stops its service: copy it from the image again.
- Update: `nft -i` is not in the image. The other `nft` commands do not
  change.
- Linux 7.3.4.
- ESP-Hosted 1.0.6.0.24. `iw dev wlan0 set power_save on` turns on the
  modem sleep of the station. Power save stays off by default.
- `/etc/os-release` has the version of the image (`IMAGE_VERSION`), and the
  loader prints the same version at boot.
- Each release has the licence texts and the sources of its images.
- The kernel links its hot path first, so its speed does not change with
  other kernel code. A pipe round trip takes about 35 % less time than in
  v0.11.0.
- Fixes in `ble-ha` (the clock after 24.8 days of uptime, the wipe of a
  Wi-Fi passphrase) and in the UART console (a hang with the interrupts off).

## v0.11.0

- Update, module alone: the overlay keeps your changed files. If you changed
  `/etc/nftables.conf` or `/etc/fstab`, merge the new file from `/rom/etc/`.
- Linux 7.3.1.
- WireGuard on all boards (`wg`, `wg-setup.sh`, `S42wireguard`).
- The access point gives addresses with DHCP and can route to `eth0` with
  NAT (`ap.sh`).
- GPIO interrupts on all pins (`gpiomon`), I2C for user space, MQTT clients
  with TLS, and a crash log of the last panic.
- More free memory: MemAvailable on an idle Korvo-1 went from 2.3 to 3.2 MiB.

## v0.10.0

- Update, module alone: the `data` partition moved. The first boot erases
  it, and the files of the overlay are lost: copy them first.
- Update, module alone: the firewall drops new connections except ping,
  DHCP, mDNS and SSH. A server on another port needs a rule.
- Bluetooth proxy and Wi-Fi setup for Home Assistant (`ble-ha`).
- `pinctrl` as on the Raspberry Pi. Thread: credential sharing, NAT64, and
  the board as the radio of Home Assistant.
- A watchdog, mDNS (`esp32s31.local`) and an SFTP server on all boards.
- iperf3 RX on the Korvo-1: about 37 Mbit/s (before 30).

## v0.9.1

- Update: only the root changed. To keep the disk, copy
  `/etc/nftables.conf` of the image and run `/etc/init.d/S35nftables start`.
- Open a port with `nft insert rule ...`, not `nft add rule ...`.
- The firewall logs its drops to `dmesg` only.

## v0.9.0

- Update: flash the new `flash.bin` on all boards, and write the new disk
  image on the Korvo-1 and the Function-CoreBoard-1.
- A firewall on the Korvo-1 and the Function-CoreBoard-1.
- Thread border router (`thread-start`, `ot-ctl`).
- `stty` on the serial console, from 9767 to 5 Mbaud.

## v0.8.1

- Update: flash the new `flash.bin` on all boards. The root password is
  locked; the consoles log in without one, and SSH takes keys.
- Module alone: Wi-Fi TX during a flash write is about 36 Mbit/s (before 25).
- Fixes: a bad frame length from the firmware no longer crashes the kernel.

## v0.8.0

- Update: the loader, OpenSBI and the kernel must come from the same
  release.
- The trap code runs from on-chip SRAM: a pipe round trip takes about 20 %
  less time.
- SD card reads on the Korvo-1: about 19 MB/s (before 6 to 9).
- About 360 KiB more RAM on each board.

## v0.7.1

- Fix: the RGB LED works again.

## v0.7.0

- `tmux` on the LCD of the Korvo-1, and more tools on the disk boards
  (`nano`, `nnn`, `ncdu`, `sqlite3`).
- The time zone database on the disk boards.

## v0.6.1

- Update, Function-CoreBoard-1: write the new USB image; the kernel finds
  its root by the disk signature.
- Fix: USB disk data was wrong in the last 4 bytes of some 4 KiB blocks.
- Fix: `eth0` on the Function-CoreBoard-1.

## v0.6.0

- Update: `make` without `BOARD` builds the module alone.
- The `coreboard1` build: root on a USB stick, Gigabit Ethernet, RGB LED.
- Drivers for the pins, the clocks and resets, and I2C.

## v0.5.0

- Update: write the new card image (a larger root and a swap partition).
  On the module alone, the first boot formats `data`.
- Update: SSH takes root only with a key.
- Linux 7.3. The module alone has a squashfs root with a writable overlay.
- WPA3 and WPA2-Enterprise, and `curl` with HTTPS.

## v0.4.0

- Update: the builds are `korvo1` and `esp32s31` (the module alone).
- Wi-Fi on the Korvo-1: about 44 Mbit/s TX and 38 Mbit/s RX.
- `poweroff` puts the chip into deep sleep.

## v0.3.1

- Update: write the new `flash.bin` and the new card image together.
- Bluetooth (`hci0`) and the 802.15.4 radio.

## v0.3.0

- Update: write the new `flash.bin` and the new card image together.
- Wi-Fi with ESP-Hosted, `wpa_supplicant` and `hostapd`.

## v0.2.2

- Fix: self-modified code (a JIT, a breakpoint) could run old code.

## v0.2.1

- Fix: about 2 % of the boots stopped early.

## v0.2.0

- Update: write the new card image; it erases the card.
- Linux is 2 to 10 times faster with the LCD (an uncached frame buffer).
- `htop`, `curl`, `httpd`, libgpiod tools and an autostart script.

## v0.1.0

- The first release.
