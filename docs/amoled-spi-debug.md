# CO5300 QSPI boot-console investigation

Repository cloned from https://github.com/pangcrd/esp32-s31-linux-amoled at
`8a0e2f2` (main). Hardware is assumed good based on the supplied ESP-IDF
PCB test.

## Status and limits

The software changes compile against Linux 7.3.4, and the device trees
compile. An opt-in kernel framebuffer console is implemented. **An actual
ESP32-S31 boot with the panel has not been verified here.** No serial
failure log, PCB access, or working board-specific ESP-IDF initialization
table was available. Therefore the original runtime root cause remains
unconfirmed; the changes below address source-level discrepancies and
make the next board run diagnostic.

The source calls the controller **CO5300** (letter O). This is assumed to
be the C05300 QSPI AMOLED mentioned in the request.

## Findings and changes

| Finding | Evidence | Change / implication |
| --- | --- | --- |
| GPSPI initialization forces no-reset while probe pulses reset | Linux set HP_SYS_CLKRST GPSPI2 bit 3, FORCE_NORST; the pinned IDF SPI reset path only asserts/deasserts bit 2 | Clear FORCE_NORST to match IDF. Its role in the reported failure must be checked on the board; do not describe it as a measured root cause yet. |
| Initialization retains reset-dependent CS timing | IDF `spi_ll_master_init()` clears CS setup/hold timing and both AFIFO error-ending enables | Write USER1/USER2 deterministically and clear interrupt enables/pending state. |
| Timeout returns only -110 | Original `esp32s31_spi_wait_transaction()` returned ETIMEDOUT for UPDATE, completion, and USR idle | Log the phase plus CMD, RAW, ST, USER, CTRL, CLOCK, MISC and DMA before recovery. Stop the state machine before GPIO CS is released. |
| A completion interrupt can arrive late | FIFO chunks repeatedly reuse one completion object | Disable and synchronize the IRQ before reinitializing completion and clearing pending flags. |
| Finished hardware can be mistaken for a hung transaction if the IRQ is lost | Original host path depended on IRQ delivery | If RAW.TRANS_DONE is set at timeout, report the CLIC issue and use bounded RAW polling for subsequent transactions. Still require CMD.USR to clear. This is a recovery path, not proof the CLIC route works. |
| Quad support was not declared for the SPI child | Original `spidev@0` had no `spi-tx-bus-width` or `spi-rx-bus-width` | Add width 4. Lowering SCLK cannot correct an unsupported lane mode. Multi-lane simultaneous TX/RX is rejected because GPSPI uses half duplex for those transfers. |
| Nothing connected printk to the AMOLED | The original child was only spidev; the coreboard1 config disabled FB/VT and selected only the UART console | Add CO5300 framebuffer driver, a separate panel DTS, and an opt-in config restoring FB/VT/fbcon and `console=tty0`. |

Existing fixes in the cloned repository were retained: reset uses
assert/deassert rather than unsupported `reset_control_reset()`, GPIO CS
has no competing native CS0 pin route, FIFOs reset before each chunk, and
CMD.USR must fall before CS is released.

### Clock and GPIO checks

The pinned ESP-IDF uses the **HP_SYS_CLKRST** HS/MST clock path, rather
than programming the legacy SPI CLK_GATE fields. Consequently
`CLK_GATE == 0` by itself is not evidence of a disabled S31 SPI clock.
The Linux BBPLL / 3 / 2 path already matches the IDF pre-divider
encoding: HS_DIV register 2 means divide by 3; MST_DIV register 1 means
divide by 2, giving an 80 MHz peripheral parent.

After removing FORCE_NORST, the programmed GPSPI2_CTRL0 values are
`0x00008163` before MST_EN and `0x00808163` after it. These are expected
configuration values, not measurements from this PCB. The source of
truth is the actual board register/clock state.

The GPIO matrix signal IDs agree with the pinned IDF. Output-enable
selection is already controlled by the peripheral for routed SPI pins;
GPIO_ENABLE being clear alone does not prove a pinctrl failure. No
speculative GPIO-output override was added, since forcing the QSPI data
pads to output would break reads.

| Signal | GPIO | Matrix signal |
| --- | ---: | ---: |
| SCLK | 45 | 53 |
| CS, active low | 46 | GPIO output |
| IO0 / D | 47 | 55 |
| IO1 / Q | 48 | 54 |
| IO2 / WP | 49 | 57 |
| IO3 / HOLD | 35 | 56 |
| Panel reset, active low | 0 | GPIO output |

These pins come from the repository. Verify them against the working
ESP-IDF program; the reset net name alone is not proof of its wiring.

## Panel transport

The new driver implements the Espressif CO5300 QSPI framing:

- Register write: `02 00 <register> 00`, then parameters, all on IO0.
- Pixel write: `32 00 2c 00` on IO0, then RGB565 pixels on four lanes.
- Header and pixel payload are two transfers in **one SPI message**, with
  `cs_change = 0`. GPIO CS remains low across both phases and all FIFO chunks.
- Pixels use MSB-first byte order. Each row is independently addressed with
  CASET/RASET so no RAMWRC/continuation behavior is assumed.
- The framebuffer is ordinary vmalloc memory. SPI uses small kmalloc
  buffers, not a direct DMA mapping of vmalloc memory.
- Drawing and mmap damage are flushed by deferred work. SPI never sleeps
  inside the fbcon drawing callbacks. Failed rows are retried with a delay.

The panel resets for at least 10 ms, then waits 150 ms after release.
The reference sequence sets MADCTL, RGB565 COLMOD, page 0, C4, brightness
and control registers, then Sleep Out with 120 ms settling and Display On.
**The module supplier's initialization may differ.**

The opt-in DTS uses the Espressif reference **466 x 466, x offset 6, y
offset 0**, and a 1 MHz SPI limit. Change `chipone,width`,
`chipone,height`, offsets and reset GPIO to match the PCB before flashing.
Framebuffer storage for that geometry is 434,312 bytes, plus deferred-I/O
metadata and fbcon allocations.

A supplier sequence can replace the default through
`chipone,init-sequence`. It is a byte array of records:

```text
command, payload_length, delay_ms_high, delay_ms_low, payload...
```

Example: `chipone,init-sequence = [fe 01 00 00 00 11 00 00 78];`
selects page 0 and sends Sleep Out with 120 ms delay. This is a format
example, not a complete recommended supplier sequence. MADCTL 0 and
COLMOD 0x55 precede the supplied sequence; Display On follows it. The
driver validates all record boundaries before executing the sequence.

## Build and enable the boot console

The default coreboard1 DTS retains spidev for transport diagnostics.
The AMOLED DTS replaces that child at the same chip select. Only one
driver may own CS0.

On the Linux build host, first satisfy the ESP-IDF/toolchain prerequisites
in README.md. From the repository root:

```sh
git submodule update --init --recursive
export ESP32S31_VERSION="$(make -s version)"
make bootloader BOARD=coreboard1
make opensbi BOARD=coreboard1

amoled_out="$PWD/build/br-output-coreboard1-amoled"
make -C external/buildroot O="$amoled_out" \
  BR2_EXTERNAL="$PWD/br2-external" coreboard1_defconfig
sh scripts/enable-amoled.sh "$amoled_out"
make -C external/buildroot O="$amoled_out" \
  BR2_EXTERNAL="$PWD/br2-external"

sh scripts/copy-images.sh "$amoled_out/images" build/coreboard1 \
  bootloader/partitions-coreboard1.csv
make -s flash-args BOARD=coreboard1 > build/coreboard1/flash-args.txt
python -m esptool --chip esp32s31 merge-bin \
  -o build/coreboard1/flash.bin @build/coreboard1/flash-args.txt
```

Run the final esptool command in the configured ESP-IDF Python environment.
Use build/coreboard1/flash.bin with the USB root image at
build/coreboard1/buildroot/sdcard.img. Partition size checks still run
through copy-images.sh. Do not use make flash for this opt-in build: its
build prerequisite invokes the default rootfs configuration again.

The helper appends `linux-amoled.fragment` after the headless and
coreboard1 fragments and cleans the kernel extraction. This is necessary:
Buildroot copies the repository's Linux files only during extraction.
After enabling, build the configured output directly. Reapplying
`coreboard1_defconfig` or running the existing `scripts/ci-build.sh`
replaces this opt-in configuration. The supplied helper does not build
or flash the board.

This displays **Linux kernel messages**, including retained printk
messages replayed when fbcon registers. ESP ROM, ESP-IDF loader and
OpenSBI messages remain on the serial console; displaying those would
also require bootloader/OpenSBI display support.

## Board validation and interpreting failures

Keep UART capture from reset. On the first Linux run, collect:

```sh
dmesg > /tmp/amoled-dmesg.txt
cat /proc/interrupts
cat /proc/cmdline
ls /sys/bus/spi/devices /sys/class/graphics
cat /sys/kernel/debug/clk/clk_summary
pinctrl get 0
pinctrl get 35
pinctrl get 45
pinctrl get 46
pinctrl get 47
pinctrl get 48
pinctrl get 49
```

The clock summary needs debugfs mounted at /sys/kernel/debug; the
coreboard1 fragment already enables CONFIG_DEBUG_FS.

| Log / symptom | What it narrows down | Next action |
| --- | --- | --- |
| `UPDATE timeout`, CMD bit 23 remains set | Controller configuration clock/reset/state-machine issue, before a wire transfer | Compare HP_SYS_CLKRST state, clock summary and reset behavior with pinned IDF. Changing panel initialization cannot fix UPDATE. |
| `TRANS_DONE timeout`, RAW bit 12 clear | Controller did not report completion | Inspect CMD.USR (bit 24), clock path, internal CS selection and DMA_CONF. Capture SCLK/CS if needed. |
| `TRANS_DONE pending without IRQ; using polling` | Shifter completed but completion IRQ was not delivered in time | Compare /proc/interrupts and CLIC source 4 to CPU input 41. Polling keeps the panel usable while the interrupt route is investigated. |
| `USR timeout` | Completion was observed, but hardware never reached idle | Inspect retained state and CS timing. Timeout recovery resets the state machine. |
| SPI calls succeed, display blank | Completion does not prove correct framing, reset, supplier init or visible window | Compare the command stream with the working ESP-IDF program, and confirm reset/geometry/brightness. |
| `spi device setup failed` / unsupported quad mode | Quad capability missing or incompatible controller mode | Verify the actual built-in DTB and the width-4 property. |
| No framebuffer | Panel driver did not probe, or headless config won | Confirm CONFIG_FB_CO5300=y, panel-compatible DTB and the helper fragment order. |
| Framebuffer exists, no kernel text | fbcon/VT or command-line configuration issue | Confirm console=tty0 and CONFIG_FRAMEBUFFER_CONSOLE=y; deferred takeover must be disabled. |

Successful acceptance requires a cold boot showing readable Linux text,
an `fb0: ... RGB565 QSPI console` registration message, no repeated
transfer timeouts, correct colors, and stable scrolling. Repeat after
several cold boots. Raise the DTS clock only after 1 MHz succeeds.

Avoid relying on an unverified panel-ID read: QSPI reads may require
supplier-specific dummy cycles and lane selection. A zero/FF read alone
does not establish a hardware fault.

## Verification performed here

- All **26 Linux patches apply to pristine 7.3.4 files with -F0**. Existing
  patches 0006, 0011, 0022, 0023 and 0025 need line offsets, but no fuzz.
- The SPI, clock and CO5300 driver objects compile with Linux 7.3.4 Kbuild,
  **W=1**, using a host x86 configuration with the required SPI/FB/clock/
  regmap dependencies. This checks C/API compatibility, not RV32 execution.
- Both coreboard1 SPI DTS and opt-in AMOLED DTS preprocess and compile
  with the kernel's dtc without warnings.
- Build helper shell syntax, repeated enabling (no duplicate fragment),
  and rejection of a non-coreboard1 output were checked. Its make calls
  were stubbed for this configuration check, not used to build images.
- Kernel checkpatch: **0 errors, 0 warnings** on the final kernel changes.
  Tracked diff whitespace checks also passed. The new panel binding parses
  successfully as YAML.
- No full Buildroot/ESP-IDF image or hardware boot has been produced.
  DT schema validation was not run because the host Python lacks pip/
  dtschema; dtc compilation and YAML parsing are separate checks.

Host setup initially lacked flex, bison, m4, bc and libelf development
headers. Tools were unpacked under /tmp for validation. An initial host
clock-object attempt lacked REGMAP configuration, and the new driver
initially needed an explicit framebuffer pointer cast; both were resolved
before the successful build. These were host/build issues, not observed
errors from the user's ESP32-S31.

## Reference sources

- Pinned ESP-IDF revision:
  [fff9895c82d744c7237be8847347bdd1b07c6643](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643)
- [ESP32-S31 SPI LL](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_hal_gpspi/esp32s31/include/hal/spi_ll.h):
  bus/function clocks, reset, master initialization, line modes and pre-dividers.
- [HP_SYS_CLKRST register definitions](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/soc/esp32s31/register/soc/hp_sys_clkrst_reg.h)
- [Espressif CO5300 SPI implementation](https://github.com/espressif/esp-iot-solution/blob/master/components/display/lcd/esp_lcd_co5300/esp_lcd_co5300_spi.c):
  QSPI opcodes, RGB565, reset delays and reference vendor commands.
