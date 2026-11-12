# ESP32-S31 Linux

English | [简体中文](README.zh-CN.md)

Linux 7.3.4 and OpenSBI 1.9 on the Espressif ESP32-S31, a microcontroller
with two RV32 harts at 320 MHz and 512 KiB of SRAM. Linux runs on hart 1
in the 16 MiB octal PSRAM. Hart 0 runs the ESP-Hosted radio firmware for
Wi-Fi, Bluetooth and Thread. A stock kernel with a small patch set boots a
Buildroot userspace: a shell, `wpa_supplicant`, BlueZ, SSH, USB and more.

```text
ESP ROM -> ESP-IDF 2nd stage -> loader (hart 0) -> OpenSBI (hart 1) -> Linux (hart 1)
```

![fastfetch on the ESP32-S31-Korvo-1](docs/fastfetch.svg)

[docs/internals.md](docs/internals.md) tells how the port works and where
the code is. [CHANGELOG.md](CHANGELOG.md) lists the changes of each
release.

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
Function-CoreBoard-1 with a cable), then `ssh root@<address>`, or use
`ssh root@esp32s31.local` (mDNS). To check the
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
| RAM for Linux (MemTotal) | about 11.6 MiB | about 11.1 MiB | 10.4 MiB |
| UART console (`ttyS0`), 9767 baud to 5 Mbaud with `stty` | yes | yes | yes |
| Copy of the boot log and JTAG on the native USB port | yes | yes | no: the LCD uses the pins |
| LCD console, USB keyboard, tmux desktop, touch (GT1151) | no | no | yes |
| Wi-Fi station (`wlan0`): WPA2 and WPA3 Personal | yes | yes | yes |
| Wi-Fi Enterprise (PEAP, TTLS, TLS) | available, not tested | available, not tested | available, not tested |
| Wi-Fi access point: WPA2 Personal (WPA3 does not work yet), DHCP for the clients | `wpa_supplicant`, `udhcpd` | `wpa_supplicant`, `hostapd`, `udhcpd` | `wpa_supplicant`, `hostapd`, `udhcpd` |
| Firewall and NAT (`nft`, rules load at boot) | yes | yes | yes |
| WireGuard VPN (`wg`, client or server) | yes | yes, also `wg-quick` | yes (a module), also `wg-quick` |
| Bluetooth (`hci0`): BlueZ, `bluetoothctl` | yes | yes | yes |
| Bluetooth proxy for Home Assistant (ESPHome API), Wi-Fi setup over Bluetooth LE (Improv) | yes | yes | yes |
| Classic BlueZ tools (`hciconfig`, `hcitool`) | no | yes | yes |
| Thread (IEEE 802.15.4): OpenThread border router for Matter, `ot-ctl`, or the radio of Home Assistant | no | yes | yes |
| Ethernet (`eth0`) | no | Gigabit | a USB adapter |
| USB host (DWC2) | no | the root stick; a second stick by hand | keyboards, mice, storage (FAT, exFAT) mounted at plug-in on `/mnt/usb`, Ethernet adapters |
| Name on the LAN (`esp32s31.local`, mDNS) | yes | yes | yes |
| Reboot after a kernel panic or a hang (soft watchdog) | yes | yes | yes |
| Crash log of the last boot after a panic or a hang (pstore) | yes | yes | yes |
| microSD | no | no | the root card |
| RGB LED (`/sys/class/leds/rgb:status`), triggers in `/etc/default/led` | no | GPIO60 | GPIO37 |
| GPIO, hardware RNG, HTTPS with `curl`, SSH (Dropbear, key only) | yes | yes | yes |
| Pin functions, pulls and levels with `pinctrl` (as on the Raspberry Pi), net names on the GPIO lines | yes | yes | yes |
| GPIO interrupts: edge events for `gpiomon` and libgpiod, debounce | yes | yes | yes |
| I2C for user space (`/dev/i2c-N`, `i2cdetect`, `i2cget`, `i2cset`) | no; a disabled example bus in `esp32s31.dts` | `i2c-0` on ESP_I2C (GPIO50/51); a disabled example on J2 pins 13 and 14 | `i2c-0`: the touch controller and the codec |

Notes:

- The Korvo-1 and the Function-CoreBoard-1 do not boot without their root
  disk. The kernel waits for it.
- `IMAGE_VERSION` in `/etc/os-release` is the version of the root. The
  loader prints the version of the flash image as `App version` at the
  start of the boot log. On the Korvo-1 and the Function-CoreBoard-1 the
  two are updated separately, and the kernel loads its modules from the
  disk: write the flash image and the disk from the same release.
- On the module alone, `flash_erase /dev/mtd1 0 0` and a reboot put the
  root back to the image. `make flash` does not write the `data` partition,
  so the overlay stays after an update.
- `poweroff` puts the chip into deep sleep. Only the reset button starts it
  again.
- Linux is uniprocessor. The FPU is not used (F without D), so userspace is
  soft-float.
- With WPA3-SAE, the first connection can fail. `wpa_supplicant` tries
  again.
- On the access point, use WPA2 (`ap.sh`). WPA3 (SAE) and protected
  management frames on the access point do not work correctly yet. The
  station is not affected.
- `hcitool lescan` fails. Use `bluetoothctl` and `scan le`.
- There is no audio or camera driver.

## Userspace

| Build | Packages |
| --- | --- |
| All boards | BusyBox, Dropbear, OpenSSL with the CA bundle, `wpa_supplicant` (WPA3, EAP, access point) with `wpa_cli` and `wpa_passphrase`, `iw`, `curl`, `iperf3`, the libgpiod 2 tools, BlueZ (`bluetoothd`, `bluetoothctl`), Lua, `nft` with a firewall, an SFTP server, `pinctrl`, `ble-ha`, `wg` |
| `esp32s31` also | The MTD tools (`mtdinfo`, `flash_erase`) |
| `coreboard1` and `korvo1` also | `mosquitto_pub`, `mosquitto_sub`, `hostapd`, the classic BlueZ tools (`hciconfig`, `hcitool`), OpenThread (`ot-daemon`, `ot-ctl`), `thread-relay`, bash, coreutils, vim, nano, less, file, htop, fastfetch, strace, gdb and `gdbserver`, tcpdump, links, MicroPython, tmux, nnn, ncdu, tree, bc, sqlite3, e2fsprogs, dosfstools |
| `coreboard1` also | `ethtool` |
| `korvo1` also | `gpm`, `irssi`, `evtest` |

`/etc/motd` lists the first commands. `/usr/share/examples` has example
scripts.

- **Wi-Fi.** `iw dev wlan0 link` shows the connection.
  `/usr/share/examples/wifi-scan.sh` lists the networks in range, and on
  the disk boards `/usr/share/examples/wifi-monitor.sh [COUNT]` shows the
  802.11 frames. On the Korvo-1, Wi-Fi gives about 45 Mbit/s TX and
  36 Mbit/s RX. While the Bluetooth proxy scans, it gives about 17/21
  Mbit/s TX/RX, and while the Thread radio receives, about 12/17.
  `/usr/share/examples/ap.sh SSID PASSPHRASE [--nat eth0]` starts a WPA2
  access point at 192.168.4.1. The clients get 192.168.4.100 to
  192.168.4.200 and reach only the board, or with `--nat` also the other
  interface. `dumpleases -f /var/run/udhcpd.wlan0.leases` lists them.
  `/etc/init.d/S40wifi restart` goes back to the station.
- **Bluetooth.** `bluetoothctl`, then `scan le`, `scan bredr`, `devices`.
  `/usr/share/examples/ble-scan.sh 10` scans for 10 seconds. `ble-ha`
  (`/etc/default/ble-ha`) gives Bluetooth LE to Home Assistant. While it
  runs, `bluetoothctl` does not work. It does not start while
  `bluetoothd` runs.
  - `/usr/share/examples/ha-bluetooth.sh HA_ADDRESS` makes the board a
    Bluetooth proxy with the ESPHome API on TCP port 6053, also at each
    boot. The API has no encryption: the firewall accepts the port only
    from the hosts in `BT_PROXY_ALLOW`. `ha-bluetooth.sh off` ends it.
  - While `wpa_supplicant` has no network, any device in range can give
    the board a network with Improv Wi-Fi over Bluetooth LE. The board
    keeps it only when it gets an address in 30 seconds. `IMPROV=no`
    turns this off.
- **Thread** (Function-CoreBoard-1, Korvo-1). `thread-start [eth0]`
  starts a border router on `wlan0` or `eth0`; `thread-start stop` ends
  it. `ot-ctl dataset init new`, `ot-ctl dataset commit active`,
  `ot-ctl ifconfig up` and `ot-ctl thread start` form a network.
  `ot-ctl state` shows it, and `ot-ctl dataset active -x` prints the
  dataset. Thread devices reach IPv4 hosts through NAT64
  (192.168.255.0/24). To give the dataset to a device:
  - chip-tool: `chip-tool pairing code-thread 1 hex:DATASET SETUPCODE`.
    On a Linux PC,
    `sysctl -w net.ipv6.conf.IFACE.accept_ra_rt_info_max_plen=64` gives
    the route to the mesh.
  - A phone app: `thread-share [MINUTES]` prints an ephemeral key, valid
    for 1 to 10 minutes (default 2). `thread-share stop` ends it.
  - Thread devices without Matter: `ot-ctl commissioner start`, then
    `ot-ctl commissioner joiner add * PSKD`.

  `/usr/share/examples/thread-hub.sh` forms a network and logs each UDP
  datagram on port 4242 to p1 of the disk.
  `/usr/share/examples/ha-thread.sh HA_ADDRESS` gives the radio to Home
  Assistant on TCP port 6638 (`THREAD_MODE=relay` and `RELAY_ALLOW` in
  `/etc/default/thread`). In the OpenThread Border Router app, set a
  dummy device and `network_device` to `BOARD_ADDRESS:6638`. The port
  gives full control of the radio: the firewall accepts it only from
  `RELAY_ALLOW`. Ethernet is better than Wi-Fi for the relay. After a
  reload of the firewall, run `thread-start stop` and `thread-start`.
  `ha-thread.sh off` ends the relay. The app of Home Assistant OS is not
  tested.
- **MQTT** (Function-CoreBoard-1, Korvo-1). `mosquitto_pub` and
  `mosquitto_sub` send and receive messages, also with TLS (port 8883;
  TLS needs the right time). `/etc/default/mqtt` (mode 600) has the
  broker, the user, the password, the client ID and the CA file.
  `/usr/share/examples/mqtt-report.sh` sends sensors to Home Assistant,
  and `/usr/share/examples/mqtt-led.sh` sets the LED from `R G B`
  messages on `esp32s31/MQTT_ID/led`. The two scripts are not tested with
  Home Assistant.
- **GPIO.** `gpiodetect`, `gpioinfo`,
  `gpioget --numeric -c gpiochip0 PIN`, `gpioset -t0 -c gpiochip0 PIN=1`,
  `/usr/share/examples/gpio.sh PIN [0|1]`. The kernel refuses the
  reserved pins. `pinctrl get 47`, `pinctrl -p get` (the header pins),
  `pinctrl funcs 8` and `pinctrl set 47 op dh` show and set the function,
  the pull and the level. `set` writes the registers past the kernel
  driver: use it only to debug. `pinctrl -h` tells more.
  `/usr/share/examples/gpio-watch.sh PIN [rising|falling|both]` prints
  each edge. `/usr/share/examples/button.sh PIN COMMAND` runs a command
  at each press of a button to GND. `gpiomon -p 20ms` ignores bounces.
- **I2C.** `i2cdetect -l` lists the buses, `i2cdetect -y 0` scans bus 0,
  `i2cget -y 0 0x48 0x00` reads a register. Scan only a bus that you
  know, and do not write to the devices of the board. On the
  Function-CoreBoard-1, bus 0 is ESP_I2C. For header J2, set
  `status = "okay"` for `&i2c1` in `esp32s31_coreboard1.dts` (SDA on pin
  13, SCL on pin 14) and add 4.7 kOhm pull-ups to 3V3. The module alone
  has an example bus on GPIO47 and GPIO48 in `esp32s31.dts`, and needs
  `CONFIG_I2C` in `linux-esp32s31.fragment`. Each change needs a new
  kernel.
- **LED** (Function-CoreBoard-1, Korvo-1).
  `/usr/share/examples/led.sh R G B` (0 to 255) sets the WS2812: it
  writes `multi_intensity` and `brightness` of
  `/sys/class/leds/rgb:status`. On the Korvo-1,
  `micropython /usr/share/examples/led.py R G B` does the same.
  `LED_TRIGGER` (`heartbeat`, `timer`, `default-on`, `netdev` with
  `LED_NETDEV`, `panic`) and `LED_COLOR` in `/etc/default/led` make it
  show a state.
- **Network.** On the Korvo-1, a USB Ethernet adapter (CDC ECM or NCM,
  ASIX AX88179, Realtek RTL8152 or RTL8153) becomes `eth0` with DHCP.
  `/usr/share/examples/report.sh URL` sends the uptime and the free
  memory with HTTPS.
- **WireGuard.** `/usr/share/examples/wg-setup.sh [PORT]` makes a key
  pair and `/etc/wireguard/wg0.conf` (mode 600), and prints the public
  key. Add the public key, the `Endpoint` and the `AllowedIPs` of the
  peer to `wg0.conf` (the format of `wg setconf`: no `Address`, `DNS` or
  `MTU`). Set `WG=yes`, `WG_ADDRESS`, `WG_ROUTES` and `WG_MTU` in
  `/etc/default/wireguard`, then run `/etc/init.d/S42wireguard restart`.
  `wg show` shows the handshake. The CPU limits the throughput.
- **Firewall.** `/etc/nftables.conf` loads at boot. The board accepts
  ping, mDNS, SSH, DHCP replies, and on the disk boards the Thread mesh
  and the border agent (UDP 49191 and 49192). These ports are open only
  while their feature runs: DHCP (UDP 67) on the access point, the
  `ListenPort` of `wg0`, TCP 6053 for `BT_PROXY_ALLOW` and TCP 6638 for
  `RELAY_ALLOW`. The firewall drops
  and logs all other new connections.
  `nft insert rule inet filter input tcp dport 8080 accept` opens a port
  until the next boot; a line in the file keeps it.
- **Web page.** Open port 8080, then
  `httpd -p 8080 -h /usr/share/examples/www` serves a page that sets the
  pins and the LED. It has no password: open the port only on a network
  that you trust.
- **Start at boot.** The board runs `autostart.sh` from p1 of the disk
  (`/mnt/sd` on the Korvo-1, `/mnt/usb` on the Function-CoreBoard-1), or
  `/root/autostart.sh`. The output is in `/tmp/autostart.log`.
  `/usr/share/examples/autostart.sh` is a template.
  `scp file root@<address>:/mnt/sd/` puts a file on p1.
- **Korvo-1.** `/usr/share/examples/touch.sh` prints the touches. In tmux
  on the LCD: `Ctrl-b c` opens a window, `Ctrl-b n` goes to the next,
  `Ctrl-b %` and `Ctrl-b "` split, `Ctrl-b d` logs out. Kernel messages go
  to `tty2` (`Alt-F2`, back with `Alt-F1`).
- **Recovery.** The board reboots 10 s after a kernel panic, and 60 s
  after user space stops (`/etc/init.d/S15watchdog stop` turns this off).
  SysRq works with `/proc/sysrq-trigger`: `s` syncs, `u` remounts
  read-only, `b` reboots.
  After a panic or a watchdog reboot, the crash log is in `crash/` on p1
  of the disk (`/root/crash` on the module alone). The reset button and a
  power cycle clear it.
- **Time.** The board has no RTC. `ntpd` uses the NTP servers from DHCP
  and `NTP_SERVERS` in `/etc/default/ntp` (`pool.ntp.org`); `NTP=no`
  turns it off. The clock is UTC. Set a local zone with a POSIX string in
  `/etc/default/tz`, for example `TZ="CET-1CEST,M3.5.0,M10.5.0/3"`, or on
  the disk boards with
  `ln -sf /usr/share/zoneinfo/Europe/Zurich /etc/localtime`.

## Updates and compatibility

From v1.0.0, the releases of one major version keep these interfaces: the
output of the `pinctrl` command, the banner line
`=== ESP32-S31 Linux / Buildroot ===`, the names in `/etc/default/*`, and
the partition table of the module alone, so that the overlay in its `data`
partition stays after an update. A release that changes one of them has a
new major version.

To update, flash `flash.bin` of the new release. On the Korvo-1 and the
Function-CoreBoard-1, also write the disk image of the same release: this
erases the files on the disk, so copy them first. The page of each release
on GitHub says what an update from the version before needs.

## Tested hardware

The port is tested on the ESP32-S31-Korvo-1 with chip revision v0.0 and
eFuse block revision v0.0, as the 2nd stage bootloader prints them in the
boot log. A different revision can change the behavior.
`python -m esptool --chip esp32s31 -p PORT chip-id` shows the revision of
your board.

The `esp32s31` and `korvo1` builds are tested on the Korvo-1. The
`coreboard1` build is tested on a Korvo-1 with the root on a USB stick;
Ethernet, the eFuse MAC address, the LED and the I2C pins are not tested
on a Function-CoreBoard-1.

| Feature | `esp32s31` | `korvo1` | `coreboard1` |
| --- | --- | --- | --- |
| Boot, console, `stty`, SysRq | tested | tested | tested on the Korvo-1 |
| Wi-Fi station | tested | tested | not tested |
| Wi-Fi access point with DHCP | not tested | not tested | not tested |
| Firewall, SSH with a key | tested | tested | not tested |
| Bluetooth, proxy for Home Assistant | not tested | tested | not tested |
| Thread border router | no | not tested | not tested |
| Writable root in the flash | tested | no | no |
| LCD, USB keyboard, touch | no | not tested | no |
| Ethernet | no | not tested | not tested |
| Watchdog, crash log | tested | tested | not tested |

## Build from source

On a macOS host, the build needs:

- Homebrew and GNU make (`brew install make`).
- ESP-IDF at `~/esp/esp-idf`, at the commit in `bootloader/esp-idf.version`,
  with the `riscv32-esp-elf` toolchain, the ESP-Hosted patches and the
  backports (`make bootloader` gives the command).
- esptool at the version in `bootloader/esptool.version`, in the Python
  environment of ESP-IDF (`make check` gives the command).
- The Apple `container` CLI. Buildroot does not run on macOS: it runs in the
  Debian image from `container/Containerfile`. Run `container system start`
  first.

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
- `make rootfs-clean BOARD=...` removes the Buildroot output of a board, for
  a build from the start.
- `scripts/reset-monitor.py --port PORT --no-reset --success-pattern ""
  --cmd COMMAND` runs shell commands on the console and stops at the first
  command that fails.
- The Korvo-1 resets each time its UART port opens.
  `scripts/reset-monitor.py --port PORT --serve 2217` keeps the port open;
  then use `rfc2217://localhost:2217` as the port of the script and of
  `make monitor`. The board resets only on an RTS pulse, and the script
  prints the time from the reset to the banner. Stop the server to flash.
- Files in `local-overlay/` go into the root of all builds. git ignores
  them.
- `make help` shows all targets.

On a Linux host, Buildroot runs without a container:

```sh
BOARD=korvo1 scripts/ci-build.sh
```

It needs ESP-IDF at `$IDF_PATH` (default `~/esp/esp-idf`), at the commit
in `bootloader/esp-idf.version`, patched with `scripts/patch-esp-idf.sh`
and with its tools installed, and the Buildroot host packages. The flash
images go to `build/<board>/`. For `korvo1` and `coreboard1`, the disk
image is `build/<board>/buildroot/sdcard.img`.

## License

`LICENSE` (MIT) applies to the build system, scripts and rootfs files. Other
files have SPDX headers: `bootloader/` and the OpenSBI platform are
BSD-2-Clause, `bootloader/components/esp_hosted/` is Apache-2.0, `linux/`
and `br2-external/package/esp-hosted-linux/` are GPL-2.0,
`shared/esp32s31-hosted-shm.h` is GPL-2.0 or Apache-2.0.

A patch (`linux/patches/`, `opensbi/patches/`, `bootloader/idf-patches/`,
`br2-external/patches/` and the patches in `br2-external/package/*/`) has
the licence of the software that it changes. `tools/pinctrl/pinctrl.c` is
BSD-3-Clause, with the copyright of Raspberry Pi Ltd. The images of a
release contain the programs of many other projects: their licence texts
are in `esp32s31-linux-VERSION-licenses.tar.xz` and their sources in
`esp32s31-linux-VERSION-sources.tar` of the same release.
