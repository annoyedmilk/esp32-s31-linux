# Mosaico peripheral test application

`mosaico-test` is a manually launched LVGL interface for ESP-Mosaico
CoreBoard V1.2. It is included in the application build, with no boot
service. Run it from the UART console or SSH:

```sh
mosaico-test
```

Tap an entry on the home screen. Home releases the current test's
resources; the close button or Ctrl-C exits and restores the display
console and keyboard mode. A small supervisor retains console ownership
so a killed GUI process can also return to the text console. Do not run
another framebuffer application at the same time.

| Entry | Operation |
| --- | --- |
| Display | Step through color bars and solid RGB, white, black and gray patterns. |
| Touch | See both CST9220 contact positions and markers. |
| Camera | Run the existing CameraBoard V1.4 / SC101IOT preview; unplugging and reconnecting the module is supported. |
| Keys & motor | Count AI key presses; explicitly request three short vibration pulses. |
| IMU | A gravity level shows tilt, with pitch/roll readouts and three signed angular-velocity bars in degrees/s. |
| Compass | Two dials show each BMM150's XY magnetic-field direction, with field strength and per-axis values in microtesla. Overflow hides invalid arrows; board magnets affect the field. This is not calibrated north. |
| Audio | Arm playback, then press AI for one 0.5 s, 440 Hz tone at the official BSP's 50% volume. A separate button records one second for microphone RMS/peak measurement without saving or playing it. |
| Wi-Fi | Show interface addresses and explicitly query the current link. Existing network settings are used. |
| Bluetooth | Explicitly start the stock lazy Bluetooth service and query the controller, without starting discovery. |
| USB | Select the data role manually and observe USB keyboard/mouse input events. Connect native Type-C to a PC for device mode, or a powered OTG hub for host mode. |
| Storage | Show writable capacity and explicitly verify a temporary 64 KiB NAND file, including `fsync`. The test does not format storage. |
| System | Show memory and swap availability. The unresponsive battery gauge is reported as unavailable. |

Audio is silent on entry. Playback needs both the Arm button and a fresh
AI press, and the speaker route is disabled when the test ends. The
board's output is quiet at this setting. The Bluetooth service remains
available after its explicit controller check, following the system's
existing service policy. Other system services are unchanged.

The interface uses charcoal surfaces, a consistent Home control and
limited accent colors for actions and measurements. Sensor plots use live
IIO values and update only while their page is open. The tilt estimate uses
acceleration as a gravity reference; movement also affects it. Gyroscope
values are not integrated into a drifting heading. The bars span +/-180
degrees/s and clip graphically while retaining the numeric value.

The display uses one eight-row RGB565 tile and a 96 KiB LVGL heap. Only
invalidated areas are written; telemetry updates at most four times per
second. Sensor plots repaint when their visible geometry or formatted
values change; there are no idle animations. Camera conversion is delegated
to `camera-preview` through an inherited framebuffer descriptor, leaving
the header and Home button visible. Camera preview still has the known
tearing limitation; this application does not introduce display vsync.

Buildroot enables the `mosaico-test` package in the application branch's
Mosaico configuration. It can be omitted through
`BR2_PACKAGE_MOSAICO_TEST`. Initialize the pinned `external/lvgl`
submodule before building. A standalone cross build can use Buildroot's
toolchain file:

```sh
cmake -S applications/mosaico-test -B build/mosaico-test \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/br-output-esp-mosaico/host/share/buildroot/toolchainfile.cmake" \
  -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build/mosaico-test
```

For host layout inspection, build with a native C compiler and ALSA
development files, then run `mosaico-test --snapshot home.ppm` or
`mosaico-test --page audio --snapshot audio.ppm`. Snapshot mode opens no
peripheral devices and runs no hardware tests. Use `--help` for page names.
