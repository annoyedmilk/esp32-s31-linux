# ESP32-S31 Linux

English | [简体中文](README.zh-CN.md)

Linux 7.3 and OpenSBI 1.9 on the Espressif ESP32-S31, a microcontroller
with two RV32 harts at 320 MHz and 512 KiB of SRAM. Linux runs on hart 1
in the 16 MiB octal PSRAM. Hart 0 runs the ESP-Hosted radio firmware for
Wi-Fi, Bluetooth and Thread. A stock kernel with a small patch set boots a
Buildroot userspace: a shell, `wpa_supplicant`, BlueZ, SSH, USB and more.

```text
ESP ROM -> ESP-IDF 2nd stage -> loader (hart 0) -> OpenSBI (hart 1) -> Linux (hart 1)
```

![fastfetch on the ESP32-S31-Korvo-1](docs/fastfetch.svg)

[docs/internals.md](docs/internals.md) tells how the port works and where
the code is. [docs/release-notes.md](docs/release-notes.md) lists the
changes of each release.

## Quick start

There are three builds. The ESP32-S31-WROOM-3-N16R16V module is the module
of the two boards.

| Board | Build | Root | Release files |
| --- | --- | --- | --- |
| ESP32-S31-WROOM-3-N16R16V module alone, on any board with UART0 | `esp32s31` (the default) | flash | `*-esp32s31-flash.zip` |
| ESP32-S31-Function-CoreBoard-1 | `coreboard1` | USB stick | `*-coreboard1-flash.zip`, `*-coreboard1-usb.img.xz` |
| ESP32-S31-Korvo-1 | `korvo1` | microSD card | `*-korvo1-flash.zip`, `*-korvo1-sdcard.img.xz` |

1. Get the files of your board from the newest
   [release](https://github.com/annoyedmilk/esp32-s31-linux/releases).
2. Install the tested esptool: `pip install "esptool==5.5.0"`
   (the version in `bootloader/esptool.version`).
3. Connect the UART port of the board. On the Korvo-1, this is the UART
   Type-C port. The USB bridge is a CP2102N on the Korvo-1 and the
   Function-CoreBoard-1. Some Function-CoreBoard-1 boards have a CH9102X.
4. Flash: `esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin`
5. Write the disk image, for example with balenaEtcher. All data on the
   disk is lost. Korvo-1: write `*-korvo1-sdcard.img.xz` to a microSD card.
   Function-CoreBoard-1: write `*-coreboard1-usb.img.xz` to a USB stick.
   Put the disk into the board. The module alone needs no disk.
6. Open a terminal on the same port at 115200 baud. Push reset. When the
   console shows `=== ESP32-S31 Linux / Buildroot ===`, you are root.

On the Korvo-1, the LCD also shows a console. Connect a USB keyboard and
log in on `tty1`: it starts a `tmux` desktop.

### Wi-Fi

```sh
wpa_passphrase "<ssid>" "<passphrase>" >> /etc/wpa_supplicant.conf
/etc/init.d/S40wifi restart
wpa_cli status
```

For WPA3 and WPA2/WPA3 networks, the network block needs
`key_mgmt=WPA-PSK SAE`, `ieee80211w=1` and the passphrase as `psk="..."`.
The configuration stays after a reboot. `S40wifi` also reads
`wpa_supplicant.conf` from p1 of the disk (the FAT partition), so a board
without a console can get its network from a computer.

### SSH

The consoles log in as root. Dropbear takes root over SSH only with a key.
On the console of the board, add your public key (one line):

```sh
mkdir -p /root/.ssh && chmod 700 /root/.ssh && echo 'ssh-ed25519 AAAA... you@host' >> /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys && sync
```

Read the address with `ip -4 addr show wlan0` (`eth0` on the
Function-CoreBoard-1 with a cable), then `ssh root@<address>`. To check the
host key, run `dropbearkey -y -f /etc/dropbear/dropbear_ed25519_host_key |
tail -1` on the console. A new disk image or `make sdroot` gives the board a
new host key and no `authorized_keys`; then run `ssh-keygen -R <address>`
on your computer. For your own builds, put the key into
`local-overlay/root/.ssh/authorized_keys`.

## Features

| Feature | `esp32s31` | `coreboard1` | `korvo1` |
| --- | --- | --- | --- |
| Root | squashfs in the flash, with a writable UBIFS overlay | ext4 on the USB stick | ext4 on the microSD card |
| Persistence | All files: the overlay is in the flash | All files on the stick | All files on the card |
| Swap | zram (6 MiB) | zram, then 64 MiB on p3 of the stick | zram, then 64 MiB on p3 of the card |
| RAM for Linux (MemTotal) | 11.9 MiB | 11.2 MiB | 10.4 MiB |
| UART console (`ttyS0`), 9767 baud to 5 Mbaud with `stty` | yes | yes | yes |
| Copy of the boot log and JTAG on the native USB port | yes | yes | no: the LCD uses the pins |
| LCD console, USB keyboard, tmux desktop, touch (GT1151) | no | no | yes |
| Wi-Fi station (`wlan0`): WPA2 and WPA3 Personal | yes | yes | yes |
| Wi-Fi Enterprise (PEAP, TTLS, TLS) | available, not tested | available, not tested | available, not tested |
| Wi-Fi access point: WPA2 Personal (WPA3 does not work yet) | `wpa_supplicant` | `wpa_supplicant`, `hostapd` | `wpa_supplicant`, `hostapd` |
| Firewall and NAT (`nft`, rules load at boot) | yes | yes | yes |
| Bluetooth (`hci0`): BlueZ, `bluetoothctl` | yes | yes | yes |
| Classic BlueZ tools (`hciconfig`, `hcitool`) | no | yes | yes |
| Thread (IEEE 802.15.4): OpenThread border router for Matter, `ot-ctl` | no | yes | yes |
| Gigabit Ethernet (`eth0`) | no | yes | no |
| USB host (DWC2) | no | the root stick; a second stick by hand | keyboards, mice, storage, mounted at plug-in |
| microSD | no | no | the root card |
| RGB LED (`/sys/class/leds/rgb:status`) | no | GPIO60 | GPIO37 |
| GPIO, hardware RNG, HTTPS with `curl`, SSH (Dropbear, key only) | yes | yes | yes |

Notes:

- The Korvo-1 and the Function-CoreBoard-1 do not boot without their root
  disk. The kernel waits for it.
- On the module alone, `flash_erase /dev/mtd1 0 0` and a reboot put the
  root back to the image. `make flash` does not write the `data` partition,
  so the overlay stays after an update. An update from v0.9.1 or older is
  the exception: the `data` partition moved, so the first boot formats it,
  and the files of the overlay are lost.
- `poweroff` puts the chip into deep sleep. Only the reset button starts it
  again.
- Linux is uniprocessor. The FPU is not used (F without D), so userspace is
  soft-float.
- With WPA3-SAE, the first connection can fail. `wpa_supplicant` tries again.
- On the access point, use WPA2 (`ap.sh`). WPA3 (SAE) and protected
  management frames on the access point do not work correctly yet. The
  station is not affected.
- `hcitool lescan` fails. Use `bluetoothctl` and `scan le`.
- There is no audio or camera driver.

## Userspace

| Build | Packages |
| --- | --- |
| All boards | BusyBox, Dropbear, OpenSSL with the CA bundle, `wpa_supplicant` (WPA3, EAP, access point) with `wpa_cli` and `wpa_passphrase`, `iw`, `curl`, `iperf3`, the libgpiod 2 tools, BlueZ (`bluetoothd`, `bluetoothctl`), Lua, `nft` with a firewall, an SFTP server |
| `esp32s31` also | The MTD tools (`mtdinfo`, `flash_erase`) |
| `coreboard1` and `korvo1` also | `hostapd`, the classic BlueZ tools (`hciconfig`, `hcitool`), OpenThread (`ot-daemon`, `ot-ctl`), bash, coreutils, vim, nano, less, file, htop, fastfetch, strace, gdb and `gdbserver`, tcpdump, links, MicroPython, tmux, nnn, ncdu, tree, bc, sqlite3, e2fsprogs, dosfstools |
| `coreboard1` also | `ethtool` |
| `korvo1` also | `gpm`, `lynx`, `irssi`, `evtest` |

`/etc/motd` lists the first commands. `/usr/share/examples` has example
scripts.

- **Wi-Fi.** `iw dev wlan0 link` shows the connection, and
  `/usr/share/examples/wifi-scan.sh` lists the networks in range with
  their signal and channel. `/usr/share/examples/ap.sh SSID PASSPHRASE`
  starts a WPA2 access point at 192.168.4.1 without DHCP.
  `/etc/init.d/S40wifi restart` goes back to the station.
- **Bluetooth.** `bluetoothctl`, then `scan le`, `scan bredr`, `devices`.
  `bluetoothd` starts at the first `bluetoothctl`.
  `/usr/share/examples/ble-scan.sh 10` lists the LE devices after a scan of
  10 seconds.
- **Thread** (Function-CoreBoard-1, Korvo-1). `thread-start` starts
  `ot-daemon` on the IEEE 802.15.4 radio as a border router on `wlan0` or
  `eth0` (`thread-start eth0` chooses); `ot-ctl` starts it at the first
  command. `ot-ctl dataset init new`, `ot-ctl dataset commit active`,
  `ot-ctl ifconfig up` and `ot-ctl thread start` form a network; `ot-ctl
  state` shows it, and `ot-ctl dataset active -x` prints the dataset for
  other devices. The border router routes IPv6 between the mesh and the
  backbone, runs the SRP and DNS-SD servers, and publishes the services
  of the Thread devices with mDNS on the backbone, so a Matter controller
  on the LAN (chip-tool, a phone) commissions Matter devices on Thread
  and talks to them through the board. The board itself announces
  `_meshcop._udp`. Wi-Fi and 802.15.4 share the radio: while the Thread
  radio receives, Wi-Fi gives approximately 12/17 Mbit/s TX/RX instead
  of 46/37.
  `thread-start stop` ends the network and the daemon. Nothing starts at
  boot. `/usr/share/examples/thread-hub.sh` is a sensor hub: it starts
  the network, prints the dataset for the sensors, and logs each UDP
  datagram on port 4242 with the time and the sender to p1 of the disk.
- **GPIO.** `gpiodetect`, `gpioinfo`, `gpioget --numeric -c gpiochip0 PIN`
  and `gpioset -t0 -c gpiochip0 PIN=1`. `/usr/share/examples/gpio.sh PIN
  [0|1]` reads or sets a pin. The kernel refuses the pins of the flash, the
  console, the loader and the drivers (`gpio-reserved-ranges` in the device
  tree).
- **LED** (Function-CoreBoard-1, Korvo-1). The WS2812 is
  `/sys/class/leds/rgb:status`: write `R G B` (0 to 255) to
  `multi_intensity`, then `255` to `brightness`.
  `/usr/share/examples/led.sh R G B` (Function-CoreBoard-1) and
  `micropython /usr/share/examples/led.py R G B` (Korvo-1) do this.
- **Network.** `iperf3` measures the throughput against a server.
  `curl` verifies certificates against the CA bundle.
  `/usr/share/examples/report.sh URL` sends the uptime and the free memory
  with HTTPS; its comment shows the `crontab` line for each 5 minutes.
- **Code.** `lua`, and `micropython` on the Function-CoreBoard-1 and the
  Korvo-1. `gdbserver` and `strace` are there too.
- **Firewall.** `/etc/nftables.conf` loads at boot: the board answers
  ping, DHCP and mDNS and takes SSH and (on the disk boards) the Thread
  mesh, and it drops and logs all other new connections. The module has
  the firewall since the release after v0.9.1: a server on another port
  needs a rule after the update. `nft
  insert rule inet filter input tcp dport 8080 accept` opens a port until the
  next boot; a line in the file keeps it. For a router between an access point and `eth0`:
  `sysctl -w net.ipv4.ip_forward=1`, then `nft add table ip nat`, `nft
  'add chain ip nat postrouting { type nat hook postrouting priority
  srcnat; }'` and `nft add rule ip nat postrouting oifname eth0
  masquerade`, and a forward rule for the access point interface.
- **Web page.** `nft insert rule inet filter input tcp dport 8080 accept`,
  then `httpd -p 8080 -h /usr/share/examples/www` serves a page that sets
  the pins and the LED. It has no password: open the port only on a
  network that you trust.
- **Start at boot.** The board runs `autostart.sh` from p1 of the disk
  (`/mnt/sd` on the Korvo-1, `/mnt/usb` on the Function-CoreBoard-1), or
  `/root/autostart.sh` when the disk has none. Its output is in
  `/tmp/autostart.log`. `/usr/share/examples/autostart.sh` is a template.
  `scp file root@<address>:/mnt/sd/` puts a file on p1.
- **Touch** (Korvo-1). `/usr/share/examples/touch.sh` prints the touches on
  the LCD with `evtest`.
- **Desktop** (Korvo-1). In tmux on the LCD: `Ctrl-b c` opens a window,
  `Ctrl-b n` goes to the next, `Ctrl-b %` and `Ctrl-b "` split, `Ctrl-b d`
  logs out.
- **Time.** The clock is UTC. On the Function-CoreBoard-1 and the Korvo-1,
  `ln -sf /usr/share/zoneinfo/Europe/Zurich /etc/localtime` sets a local
  zone.

## Tested hardware

The port is tested on the ESP32-S31-Korvo-1 with chip revision v0.0 and
eFuse block revision v0.0, as the 2nd stage bootloader prints them in the
boot log. A different revision can change the behavior.
`python -m esptool --chip esp32s31 -p PORT chip-id` shows the revision of
your board.

## Build from source

The host is macOS. The build needs:

- Homebrew and GNU make (`brew install make`).
- ESP-IDF at `~/esp/esp-idf`, at the commit in `bootloader/esp-idf.version`,
  with the `riscv32-esp-elf` toolchain, the ESP-Hosted patches and the
  backports (`make bootloader` gives the command).
- esptool at the version in `bootloader/esptool.version`, in the Python
  environment of ESP-IDF (`make check` gives the command).
- The Apple `container` CLI. Buildroot does not run on macOS: it runs in the
  Debian image from `container/Containerfile`. Run `container system start`
  first.

On Linux, `scripts/ci-build.sh` builds everything without a container.

```sh
git submodule update --init --recursive
make container-image
make check
make ports
make build   BOARD=korvo1
make flash   BOARD=korvo1 FLASH_PORT=/dev/cu.usbserial-XXXX
make monitor BOARD=korvo1 SERIAL_PORT=/dev/cu.usbserial-XXXX
```

- `BOARD` selects the build for each target: `esp32s31` (the default),
  `coreboard1` or `korvo1`. The output goes to `build/<board>/`.
- `make build` builds the loader and OpenSBI on the Mac, and the kernel, the
  rootfs and the disk images in the container.
- `make monitor` resets the board, waits for the banner, then stays open as
  a terminal. Push `Ctrl-]` to exit. Set `RESET_PORT` if the RTS line of
  `SERIAL_PORT` cannot reset the board.
- `make sdwrite SD_DISK=/dev/diskN` writes the disk image to a card or a
  stick. `make sdpart` lays out a large disk. `make sdroot` writes only the
  root partition, for an update. These targets erase data and ask first.
- Files in `local-overlay/` go into the root of all builds. git ignores them.
- `make help` shows all targets.

## License

`LICENSE` (MIT) applies to the build system, scripts and rootfs files. Other
files have SPDX headers: `bootloader/` and the OpenSBI platform are
BSD-2-Clause, `bootloader/components/esp_hosted/` is Apache-2.0, `linux/`
and `br2-external/package/esp-hosted-linux/` are GPL-2.0,
`shared/esp32s31-hosted-shm.h` is GPL-2.0 or Apache-2.0.
