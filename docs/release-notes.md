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

## Known limits

- The board and chip revision v0.0 are the only tested hardware.
- One hart runs Linux. The other runs the Wi-Fi firmware.
- Floating point works in software; the hardware FPU is not used.
- No I2C, audio or camera driver.
- `poweroff` stops the CPU. It does not remove power.

See the README for the full list, and `docs/internals.md` for the details.
