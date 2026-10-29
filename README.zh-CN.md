# ESP32-S31 Linux

[English](README.md) | 简体中文

> 本文译自 [README.md](README.md) 的提交 `96f2c15`。两者不同时，以英文版为准。

在乐鑫 ESP32-S31 上运行 Linux 7.3 和 OpenSBI 1.9。ESP32-S31 是一款微控制器，
有两个 320 MHz 的 RV32 hart 和 512 KiB SRAM。Linux 在 hart 1 上运行，使用
16 MiB 八线 PSRAM。hart 0 运行 ESP-Hosted 无线固件，提供 Wi-Fi、蓝牙和
Thread。标准内核加上一小组补丁即可启动 Buildroot 用户空间：shell、
`wpa_supplicant`、BlueZ、SSH、USB 等。

```text
ESP ROM -> ESP-IDF 2nd stage -> loader (hart 0) -> OpenSBI (hart 1) -> Linux (hart 1)
```

![ESP32-S31-Korvo-1 上的 fastfetch](docs/fastfetch.svg)

[docs/internals.md](docs/internals.md)（英文）说明移植的工作原理和代码位置。
[docs/release-notes.md](docs/release-notes.md)（英文）列出每个版本的更改。

## 快速开始

共有三种构建。ESP32-S31-WROOM-3-N16R16V 模组是两块开发板上使用的模组。

| 开发板 | 构建 | 根文件系统 | 发布文件 |
| --- | --- | --- | --- |
| 单独使用 ESP32-S31-WROOM-3-N16R16V 模组，任何带 UART0 的板子均可 | `esp32s31`（默认） | Flash | `*-esp32s31-flash.zip` |
| ESP32-S31-Function-CoreBoard-1 | `coreboard1` | U 盘 | `*-coreboard1-flash.zip`、`*-coreboard1-usb.img.xz` |
| ESP32-S31-Korvo-1 | `korvo1` | microSD 卡 | `*-korvo1-flash.zip`、`*-korvo1-sdcard.img.xz` |

1. 从最新的[发布版本](https://github.com/annoyedmilk/esp32-s31-linux/releases)
   下载对应开发板的文件。
2. 安装经过测试的 esptool：`pip install "esptool==5.5.0"`
   （版本见 `bootloader/esptool.version`）。
3. 连接开发板的 UART 端口。Korvo-1 上是 UART Type-C 口。Korvo-1 和
   Function-CoreBoard-1 的 USB 转串口芯片是 CP2102N，部分
   Function-CoreBoard-1 使用 CH9102X。
4. 烧录：`esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin`
5. 写入磁盘镜像，例如使用 balenaEtcher。磁盘上的所有数据都会丢失。
   Korvo-1：将 `*-korvo1-sdcard.img.xz` 写入 microSD 卡。
   Function-CoreBoard-1：将 `*-coreboard1-usb.img.xz` 写入 U 盘。
   然后将磁盘插入开发板。单独使用模组时不需要磁盘。
6. 在同一端口打开终端，波特率 115200。按复位键。控制台显示
   `=== ESP32-S31 Linux / Buildroot ===` 后，即以 root 身份登录。

Korvo-1 的 LCD 也显示一个控制台。连接 USB 键盘并在 `tty1` 上登录，会启动
一个 `tmux` 桌面。

### Wi-Fi

```sh
wpa_passphrase "<ssid>" "<passphrase>" >> /etc/wpa_supplicant.conf
/etc/init.d/S40wifi restart
wpa_cli status
```

对于 WPA3 和 WPA2/WPA3 网络，network 块需要 `key_mgmt=WPA-PSK SAE`、
`ieee80211w=1`，并以 `psk="..."` 的形式写入密码。配置在重启后保留。
`S40wifi` 也会读取磁盘 p1（FAT 分区）上的 `wpa_supplicant.conf`，因此没有
控制台的开发板也可以从电脑获取网络配置。

### SSH

控制台以 root 身份登录。Dropbear 只允许 root 使用密钥通过 SSH 登录。
在开发板的控制台上添加你的公钥（一行）：

```sh
mkdir -p /root/.ssh && chmod 700 /root/.ssh && echo 'ssh-ed25519 AAAA... you@host' >> /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys && sync
```

用 `ip -4 addr show wlan0` 查看地址（Function-CoreBoard-1 接网线时用
`eth0`），然后运行 `ssh root@<address>`。要核对主机密钥，在控制台上运行
`dropbearkey -y -f /etc/dropbear/dropbear_ed25519_host_key | tail -1`。
写入新的磁盘镜像或运行 `make sdroot` 后，开发板会生成新的主机密钥，且没有
`authorized_keys`；此时在电脑上运行 `ssh-keygen -R <address>`。自行构建时，
将密钥放入 `local-overlay/root/.ssh/authorized_keys`。

## 功能

| 功能 | `esp32s31` | `coreboard1` | `korvo1` |
| --- | --- | --- | --- |
| 根文件系统 | Flash 中的 squashfs，带可写的 UBIFS overlay | U 盘上的 ext4 | microSD 卡上的 ext4 |
| 持久化 | 所有文件：overlay 位于 Flash 中 | U 盘上的所有文件 | 卡上的所有文件 |
| Swap | zram（6 MiB） | 先 zram，再用 U 盘 p3 上的 64 MiB | 先 zram，再用卡 p3 上的 64 MiB |
| Linux 可用内存（MemTotal） | 11.9 MiB | 11.2 MiB | 10.4 MiB |
| UART 控制台（`ttyS0`），可用 `stty` 设为 9767 波特至 5 Mbaud | 是 | 是 | 是 |
| 原生 USB 口上的启动日志副本和 JTAG | 是 | 是 | 否：LCD 占用这些引脚 |
| LCD 控制台、USB 键盘、tmux 桌面、触摸（GT1151） | 否 | 否 | 是 |
| Wi-Fi station（`wlan0`）：WPA2 和 WPA3 个人版 | 是 | 是 | 是 |
| Wi-Fi 企业版（PEAP、TTLS、TLS） | 可用，未测试 | 可用，未测试 | 可用，未测试 |
| Wi-Fi 接入点：WPA2 个人版（WPA3 暂不可用） | `wpa_supplicant` | `wpa_supplicant`、`hostapd` | `wpa_supplicant`、`hostapd` |
| 防火墙和 NAT（`nft`，规则在启动时加载） | 是 | 是 | 是 |
| 蓝牙（`hci0`）：BlueZ、`bluetoothctl` | 是 | 是 | 是 |
| 传统 BlueZ 工具（`hciconfig`、`hcitool`） | 否 | 是 | 是 |
| Thread（IEEE 802.15.4）：用于 Matter 的 OpenThread 边界路由器，`ot-ctl` | 否 | 是 | 是 |
| 千兆以太网（`eth0`） | 否 | 是 | 否 |
| USB 主机（DWC2） | 否 | 根文件系统所在的 U 盘；第二个 U 盘需手动挂载 | 键盘、鼠标、存储设备，插入时自动挂载 |
| microSD | 否 | 否 | 根文件系统所在的卡 |
| RGB LED（`/sys/class/leds/rgb:status`） | 否 | GPIO60 | GPIO37 |
| GPIO、硬件随机数、`curl` HTTPS、SSH（Dropbear，仅密钥） | 是 | 是 | 是 |

说明：

- Korvo-1 和 Function-CoreBoard-1 没有根磁盘时无法启动，内核会一直等待磁盘。
- 单独使用模组时，运行 `flash_erase /dev/mtd1 0 0` 再重启，可将根文件系统
  恢复为镜像状态。`make flash` 不写入 `data` 分区，因此更新后 overlay 保留。
  从 v0.9.1 或更早版本更新时例外：`data` 分区的位置改变，首次启动会格式化它，
  overlay 中的文件会丢失。
- `poweroff` 使芯片进入 Deep-sleep 模式，只有复位键能再次启动它。
- Linux 以单处理器模式运行。不使用 FPU（只有 F，没有 D），因此用户空间使用
  软浮点。
- 使用 WPA3-SAE 时，第一次连接可能失败，`wpa_supplicant` 会自动重试。
- 接入点请使用 WPA2（`ap.sh`）。接入点上的 WPA3（SAE）和受保护管理帧
  （PMF）暂时还不能正常工作。station 模式不受影响。
- `hcitool lescan` 无法使用。请使用 `bluetoothctl` 和 `scan le`。
- 没有音频和摄像头驱动。

## 用户空间

| 构建 | 软件包 |
| --- | --- |
| 所有开发板 | BusyBox、Dropbear、带 CA 证书包的 OpenSSL、`wpa_supplicant`（WPA3、EAP、接入点）及 `wpa_cli` 和 `wpa_passphrase`、`iw`、`curl`、`iperf3`、libgpiod 2 工具、BlueZ（`bluetoothd`、`bluetoothctl`）、Lua、带防火墙的 `nft`、SFTP 服务器 |
| `esp32s31` 另有 | MTD 工具（`mtdinfo`、`flash_erase`） |
| `coreboard1` 和 `korvo1` 另有 | `hostapd`、传统 BlueZ 工具（`hciconfig`、`hcitool`）、OpenThread（`ot-daemon`、`ot-ctl`）、bash、coreutils、vim、nano、less、file、htop、fastfetch、strace、gdb 和 `gdbserver`、tcpdump、links、MicroPython、tmux、nnn、ncdu、tree、bc、sqlite3、e2fsprogs、dosfstools |
| `coreboard1` 另有 | `ethtool` |
| `korvo1` 另有 | `gpm`、`lynx`、`irssi`、`evtest` |

`/etc/motd` 列出入门命令。`/usr/share/examples` 中有示例脚本。

- **Wi-Fi。** `iw dev wlan0 link` 显示连接状态，
  `/usr/share/examples/wifi-scan.sh` 列出范围内的网络及其信号强度和信道。
  `/usr/share/examples/ap.sh SSID PASSPHRASE` 在 192.168.4.1 启动 WPA2
  接入点，不提供 DHCP。`/etc/init.d/S40wifi restart` 切回 station 模式。
- **蓝牙。** 运行 `bluetoothctl`，然后 `scan le`、`scan bredr`、`devices`。
  `bluetoothd` 在第一次运行 `bluetoothctl` 时启动。
  `/usr/share/examples/ble-scan.sh 10` 扫描 10 秒后列出 LE 设备。
- **Thread**（Function-CoreBoard-1、Korvo-1）。`thread-start` 在
  IEEE 802.15.4 无线上启动 `ot-daemon`，作为 `wlan0` 或 `eth0` 上的边界
  路由器（用 `thread-start eth0` 选择）；第一次运行 `ot-ctl` 命令时也会启动它。
  `ot-ctl dataset init new`、`ot-ctl dataset commit active`、
  `ot-ctl ifconfig up` 和 `ot-ctl thread start` 组建网络；`ot-ctl state`
  显示状态，`ot-ctl dataset active -x` 打印供其他设备使用的数据集。边界路由器
  在 Mesh 网络和骨干网络之间路由 IPv6，运行 SRP 和 DNS-SD 服务器，并通过
  mDNS 在骨干网络上发布 Thread 设备的服务，因此局域网中的 Matter 控制器
  可以通过开发板与 Thread 上的 Matter 设备通信。开发板自身广播
  `_meshcop._udp`。为 Matter 设备配网时，控制器通过蓝牙把数据集交给设备：
  - PC 上的 chip-tool 直接使用数据集：
    `chip-tool pairing code-thread 1 hex:DATASET SETUPCODE`。在 Linux 上，
    `sysctl -w net.ipv6.conf.IFACE.accept_ra_rt_info_max_plen=64` 让 PC
    获得到 Mesh 网络的路由。
  - 手机应用（Google Home、Apple Home）只从自己的凭据存储中获取数据集。
    `thread-share` 启动一个临时密钥（ePSKc，Thread 1.4 凭据共享），有效
    2 分钟，并打印该密钥；支持凭据共享的应用可用它获取数据集。
  - 不使用 Matter 的 Thread 设备用 PSKd 加入：`ot-ctl commissioner start`，
    然后 `ot-ctl commissioner joiner add * PSKD`。

  Wi-Fi 和 802.15.4 共用射频：Thread 无线
  接收时，Wi-Fi 吞吐量（发送/接收）约为 12/17 Mbit/s，而不是 46/37 Mbit/s。`thread-start stop`
  停止网络和守护进程。开机时不会自动启动。`/usr/share/examples/thread-hub.sh`
  是一个传感器集线器：它启动网络，打印供传感器使用的数据集，并将 UDP 4242
  端口收到的每个数据报连同时间和发送方记录到磁盘 p1 上。
- **GPIO。** `gpiodetect`、`gpioinfo`、`gpioget --numeric -c gpiochip0 PIN`
  和 `gpioset -t0 -c gpiochip0 PIN=1`。`/usr/share/examples/gpio.sh PIN [0|1]`
  读取或设置引脚。内核拒绝使用 Flash、控制台、加载程序和驱动占用的引脚
  （设备树中的 `gpio-reserved-ranges`）。
- **LED**（Function-CoreBoard-1、Korvo-1）。WS2812 对应
  `/sys/class/leds/rgb:status`：向 `multi_intensity` 写入 `R G B`（0 到 255），
  再向 `brightness` 写入 `255`。`/usr/share/examples/led.sh R G B`
  （Function-CoreBoard-1）和 `micropython /usr/share/examples/led.py R G B`
  （Korvo-1）完成同样的操作。
- **网络。** `iperf3` 测量与服务器之间的吞吐量。`curl` 使用 CA 证书包验证证书。
  `/usr/share/examples/report.sh URL` 通过 HTTPS 发送运行时间和空闲内存；
  脚本注释中给出每 5 分钟运行一次的 `crontab` 行。
- **编程。** `lua`，Function-CoreBoard-1 和 Korvo-1 上还有 `micropython`。
  也有 `gdbserver` 和 `strace`。
- **防火墙。** `/etc/nftables.conf` 在启动时加载：开发板响应 ping、DHCP 和
  mDNS，接受 SSH 和（磁盘开发板上的）Thread Mesh 流量，丢弃并
  记录所有其他新连接。模组从 v0.9.2 起也有防火墙：更新后，
  其他端口上的服务器需要一条规则。
  `nft insert rule inet filter input tcp dport 8080 accept`
  打开一个端口，直到下次重启；在该文件中加一行可永久保留。在接入点和 `eth0`
  之间做路由器：`sysctl -w net.ipv4.ip_forward=1`，然后
  `nft add table ip nat`、
  `nft 'add chain ip nat postrouting { type nat hook postrouting priority srcnat; }'`
  和 `nft add rule ip nat postrouting oifname eth0 masquerade`，并为接入点
  接口添加一条 forward 规则。
- **网页。** `nft insert rule inet filter input tcp dport 8080 accept`，然后
  `httpd -p 8080 -h /usr/share/examples/www` 提供一个可设置引脚和 LED 的网页。
  网页没有密码：只在可信的网络中打开该端口。
- **开机自启动。** 开发板运行磁盘 p1 上的 `autostart.sh`（Korvo-1 上为
  `/mnt/sd`，Function-CoreBoard-1 上为 `/mnt/usb`）；磁盘上没有时，运行
  `/root/autostart.sh`。输出保存在 `/tmp/autostart.log`。
  `/usr/share/examples/autostart.sh` 是模板。
  `scp file root@<address>:/mnt/sd/` 可将文件复制到 p1。
- **触摸**（Korvo-1）。`/usr/share/examples/touch.sh` 用 `evtest` 打印 LCD
  上的触摸事件。
- **桌面**（Korvo-1）。在 LCD 上的 tmux 中：`Ctrl-b c` 打开新窗口，
  `Ctrl-b n` 切换到下一个窗口，`Ctrl-b %` 和 `Ctrl-b "` 分屏，`Ctrl-b d`
  退出登录。内核消息不会覆盖桌面：它们显示在 `tty2`（`Alt-F2`，用 `Alt-F1`
  返回）和 UART 上。
- **时间。** 时钟为 UTC。在 Function-CoreBoard-1 和 Korvo-1 上，
  `ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime` 设置本地时区。

## 已测试的硬件

本移植在 ESP32-S31-Korvo-1 上测试，芯片版本 v0.0，eFuse 块版本 v0.0
（以二级引导加载程序在启动日志中打印的为准）。不同版本可能导致行为不同。
`python -m esptool --chip esp32s31 -p PORT chip-id` 显示开发板的版本。

## 从源码构建

主机为 macOS。构建需要：

- Homebrew 和 GNU make（`brew install make`）。
- ESP-IDF，位于 `~/esp/esp-idf`，检出到 `bootloader/esp-idf.version` 中的
  提交，并带有 `riscv32-esp-elf` 工具链、ESP-Hosted 补丁和向后移植补丁
  （`make bootloader` 会给出命令）。
- esptool，版本见 `bootloader/esptool.version`，安装在 ESP-IDF 的 Python
  环境中（`make check` 会给出命令）。
- Apple `container` CLI。Buildroot 无法在 macOS 上运行，它在
  `container/Containerfile` 构建的 Debian 镜像中运行。请先运行
  `container system start`。

在 Linux 上，`scripts/ci-build.sh` 无需容器即可构建全部内容。

```sh
git submodule update --init --recursive
make container-image
make check
make ports
make build   BOARD=korvo1
make flash   BOARD=korvo1 FLASH_PORT=/dev/cu.usbserial-XXXX
make monitor BOARD=korvo1 SERIAL_PORT=/dev/cu.usbserial-XXXX
```

- `BOARD` 选择目标板的构建：`esp32s31`（默认）、`coreboard1` 或 `korvo1`。
  输出位于 `build/<board>/`。
- `make build` 在 Mac 上构建加载程序和 OpenSBI，在容器中构建内核、rootfs
  和磁盘镜像。
- `make monitor` 复位开发板，等待启动横幅，然后作为终端保持打开。按 `Ctrl-]`
  退出。如果 `SERIAL_PORT` 的 RTS 线无法复位开发板，请设置 `RESET_PORT`。
- `make sdwrite SD_DISK=/dev/diskN` 将磁盘镜像写入卡或 U 盘。`make sdpart`
  为大容量磁盘分区。`make sdroot` 只写入根分区，用于更新。这些目标会擦除数据，
  执行前会先询问。
- `local-overlay/` 中的文件会放入所有构建的根目录。git 忽略这些文件。
- `make help` 显示所有目标。

## 许可证

`LICENSE`（MIT）适用于构建系统、脚本和 rootfs 文件。其他文件带有 SPDX 头：
`bootloader/` 和 OpenSBI 平台代码为 BSD-2-Clause，
`bootloader/components/esp_hosted/` 为 Apache-2.0，`linux/` 和
`br2-external/package/esp-hosted-linux/` 为 GPL-2.0，
`shared/esp32s31-hosted-shm.h` 为 GPL-2.0 或 Apache-2.0。
