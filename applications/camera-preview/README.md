# Camera preview

`camera-preview` displays CameraBoard V1.4 / SC101IOT capture on the
ESP-Mosaico CoreBoard V1.2 display. It uses V4L2 MMAP buffers and fbdev,
converts 640x480 UYVY to RGB565, and fits a 480x360 preview between black
borders on the 480x480 screen. No service starts it automatically.

Conversion uses an eight-line RGB tile (7.5 KiB) and writes it to fbdev
before converting the next tile. `fsync` waits for the deferred display
update after each frame.

Run as root from the UART console or SSH:

```sh
camera-preview
# Select different device nodes when needed:
camera-preview -c /dev/video0 -f /dev/fb0
```

Press Ctrl-C to stop and restore the display console. Only one preview
should use the display at a time; an advisory framebuffer lock prevents
another instance of this program from taking it.

The program shows `WAITING FOR CAMERA` while the module is absent. After
unplugging or a capture timeout it releases the old queue, then reopens
the device every 750 ms until the module returns. Frame processing stays
idle while waiting. A fatal DMA-stop failure requires a board reboot.

The current display path has no tearing synchronization. Moving images
can show tearing; dropping older capture buffers reduces latency but
does not synchronize display scanning. This application preserves that
known limitation.

Build separately with `make` (or `make CC=<cross-compiler>`). Buildroot
packages it when `BR2_PACKAGE_CAMERA_PREVIEW` is enabled. No audio or
other peripheral tests run as part of this application.
