Linux 7.2 on the ESP32-S31-Korvo-1, ready to flash.

## Files

| File | Use |
| --- | --- |
| `esp32s31-linux-*-flash.zip` | Flash images. `flash.bin` goes to offset 0. `FLASH.txt` tells how. |
| `esp32s31-linux-*-sdcard.img.xz` | microSD card image: FAT for your files and the ext4 root. |
| `SHA256SUMS` | Checksums of the two files. |

## Quick start

1. `pip install "esptool>=5.4"`
2. Connect the UART Type-C port of the board.
3. `esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin`
4. Write the card image to a microSD card (all data on the card is lost),
   for example with balenaEtcher. Put the card into the board.
5. Open the console on the same port at 115200 baud, and push the reset
   button. The board logs in as root.
6. Wi-Fi: `wpa_passphrase "<ssid>" "<passphrase>" >> /etc/wpa_supplicant.conf`,
   then `/etc/init.d/S40wifi restart`.

The LCD shows the kernel log and a login. A USB keyboard works on it.

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

- The board and chip revision v0.0 are the only tested hardware.
- One hart runs Linux. The other runs the Wi-Fi firmware.
- Floating point works in software; the hardware FPU is not used.
- No I2C, audio or camera driver.
- `poweroff` stops the CPU. It does not remove power.

See the README for the full list, and `docs/internals.md` for the details.
