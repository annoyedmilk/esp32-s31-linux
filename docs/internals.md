# ESP32-S31 internals for Linux developers

This document tells how Linux runs on the ESP32-S31, and why the port has
its current form. It is for developers who know the RISC-V Linux kernel and
want to understand this SoC. All facts are from chip revision v0.0 on the
ESP32-S31-Korvo-1 board. Most of them were measured with JTAG, because
Espressif does not publish a technical reference manual for this chip yet.
The port has three builds: `esp32s31` for the ESP32-S31-WROOM-3-N16R16V
module alone, `coreboard1` for the ESP32-S31-Function-CoreBoard-1, and
`korvo1` for the Korvo-1. The two boards use that module.

The ESP-IDF register headers (`components/soc/esp32s31/register/soc/`) are
the best public reference for addresses and fields.

## The SoC from the view of Linux

| Item | Value |
| --- | --- |
| Harts | 2 × RV32 at 320 MHz: `rv32imafc` + Zba/Zbb/Zbs, Zc*, vendor extensions |
| Privilege modes | M, S, U. Sv32 MMU. |
| FPU | F only, no D. Linux supports an FPU only with D, so the FPU stays off. Floating point works in software (soft-float userspace), only slower. |
| Cache | I-cache per hart, one 64 KiB D-cache for the two harts, 64-byte lines |
| Cache maintenance | No Zicbom. A cache controller with a "sync engine" (MMIO). |
| Interrupts | CLIC only. The standard `sie`/`sip` CSRs do not exist. |
| Internal SRAM | 512 KiB at `0x2F000000`, not cached by the D-cache |
| PSRAM | 16 MiB octal, cached. The loader maps it at `0x50000000` with the MMU of the PSRAM controller. Linux uses it as RAM. An uncached alias is at `0xC0000000`. |
| Flash | 16 MB NOR, mapped by the MMU of the flash controller |

## Boot chain

```text
ROM -> ESP-IDF 2nd stage -> loader (hart 0) -> OpenSBI (hart 1, M) -> Linux (hart 1, S)
```

1. The ESP-IDF second-stage bootloader starts the loader application
   (`bootloader/main/`) on hart 0.
2. The loader starts PSRAM, and on the Korvo-1 the LCD
   (`board_korvo1.c`). It copies two flash partitions
   to PSRAM and checks them: the kernel `Image` (size and CRC32 from a
   manifest at the end of the kernel partition) and OpenSBI `fw_jump`. It
   starts Wi-Fi and Bluetooth.
3. The loader writes the D-cache back, sets the PMA of hart 1 for PSRAM and
   releases hart 1 at the OpenSBI entry. Then it locks hart 0 out of PSRAM
   and the DMA pool with PMP.
4. Hart 0 stays in the loader. The loader is now the ESP-Hosted Wi-Fi
   firmware.
5. OpenSBI sets up the CLIC, the timer, PMP, APM and the SBI extensions.
   Then it jumps to the kernel in S-mode.
6. The kernel has a built-in DTB (`esp32s31.dts`,
   `esp32s31_coreboard1.dts` or `esp32s31_korvo1.dts`). On the Korvo-1,
   the root is the ext4 partition `/dev/mmcblk0p2` on the SD card, on the
   Function-CoreBoard-1 p2 of the USB stick with the disk signature
   `0x45533331` (`root=PARTUUID=45533331-02` and `rootwait`). On the
   esp32s31 board, the root is the squashfs in the flash, with a UBIFS
   overlay.

### Memory map

| Address | Size | Use |
| --- | --- | --- |
| `0x2F040000` | 64 KiB | Coherent DMA pool for Linux (`shared-dma-pool`) |
| `0x2F050000` | 52 KiB | Shared block: requests, flash mailbox, ESP-Hosted rings |
| `0x2F079000` | 4 KiB | LCD DMA descriptor ring (Korvo-1) |
| `0x50000000` | 15 MiB - 4 KiB | Linux RAM, from the kernel image |
| `0x50EFF000` | 4 KiB | Guard page below OpenSBI (reserved, no-map, see Protection) |
| `0x50F00000` | 256 KiB | OpenSBI (reserved, no-map) |
| `0x50F40000` | 768 KiB | LCD frame buffer (Korvo-1: reserved, no-map, uncached; other boards: Linux RAM) |

The loader keeps its own copy of these numbers. The device tree
(`esp32s31.dtsi`) must agree with it.

### Internal SRAM

The HP SRAM is 512 KiB (`0x2F000000`-`0x2F080000`). Measured on the
Korvo-1 with Wi-Fi and Bluetooth, at v0.4.0 (the numbers change with the
firmware):

| Use | Size |
| --- | --- |
| Loader and ESP-Hosted firmware, static (`.text`, `.data`, `.bss`) | 165 KiB |
| Linux: DMA pool and shared block (`0x2F040000`-`0x2F05D000`) | 116 KiB |
| LCD descriptor ring and reserved blocks | 8 KiB |
| ESP-IDF heap in HP SRAM | 211 KiB |

The heap also has 32 KiB of LP SRAM. Of the controllers, Bluetooth takes the
most: 42 KiB of code in SRAM and 61 KiB of heap. 100 KiB of heap are free
when hart 1 starts. With 32 dynamic Wi-Fi buffers in each direction, the
minimum was 35.9 KiB during iperf3 with a BLE scan and a BR/EDR inquiry.

## Why Linux uses only hart 1

The Wi-Fi and Bluetooth radios need the ESP-IDF libraries. These libraries
are binary, run in M-mode and use FreeRTOS. So hart 0 keeps a small
ESP-IDF application that owns the radio, the ESP-Hosted firmware, and
Linux talks to it through shared SRAM (see "Wi-Fi"). The price is a
uniprocessor Linux.

## Interrupts: the CLIC

The hart has only a CLIC (Core-Local Interrupt Controller). There is no
PLIC, and the S-mode interrupt CSRs (`sie`, `sip`) are illegal
instructions. The Interrupt Matrix connects the SoC interrupt sources to
the 32 external CLIC inputs (IDs 16 to 47).

Each hart sees its own CLIC registers at the same address. The registers of
the other hart are at `+0x10000`.

| Window | Address |
| --- | --- |
| M-mode configuration (`cliccfg`) | `0x10800000` |
| M-mode per-input registers (IP, IE, ATTR, CTL: 4 bytes per ID) | `0x10801000` |
| S-mode configuration | `0x10A00000` |
| S-mode per-input registers | `0x10A01000` |

The traps that cost the most time:

- **An input is visible to S-mode only when M-mode gives it.** The MODE
  field of `clicintattr` must be S. Otherwise the S-mode window reads 0 and
  ignores writes, with no fault. OpenSBI gives inputs 16 to 47 to S-mode.
- **`cliccfg` is per hart.** Write the full value. The Linux hart does not
  run the ESP-IDF start code. The measured layout is `mnlbits` [3:0],
  `nmbits` [5:4], `snlbits` [19:16], `unlbits` [27:24]. The ESP-IDF header
  `clic_reg.h` shows an older layout. OpenSBI writes `0x23`: three machine
  level bits, `nmbits` = 2 (the MODE field selects M, S or U), and no
  supervisor level bits, so all S-mode inputs have level 255.
- **`sret` restores the interrupt level from `scause.spil`, but only when
  `scause` bit 31 is set.** With an exception-type `scause`, the level stays
  where it is. `mret` from S-mode does not trap on this revision. It changes
  only the machine level and leaves `sintstatus.SIL` high, which masks all
  S-mode interrupts.
- **The level stack and the scheduler.** Only `sret` lowers the level. If a
  handler schedules another task while its level is still high, that task
  runs with interrupts masked. The stock `handle_exception` (with the hunks
  of `linux/patches/0001`) does an `sret` to itself before `do_irq()`, with
  `spil` = 0, so that all handlers run at the base level.
- **`scause` holds SPP and SPIE.** In CLIC mode, these bits of `scause` are
  aliases of the same bits in `sstatus`. A write of `scause` changes them:
  with 0, `sret` returns in user mode.
- **Exceptions come to the same vector.** In CLIC mode, `stvec` catches
  exceptions and interrupts, so `handle_exception` is 64-byte aligned. It
  masks the CLIC bits out of `scause` before the usual exception handlers.

Driver: `drivers/irqchip/irq-esp32s31-clic.c`. Interrupt specifiers have
three cells: `<CLIC ID, matrix source, type>`. The driver writes the
Interrupt Matrix route itself.

### Timer and IPI

The machine timer is CLINT-like at `0x10000000` (`mtime` at `+0xBFF8`,
`mtimecmp` at `+0x4000`). A match is CLIC input 7 (M-mode). OpenSBI takes
it and sets input 5, which is the S-mode timer ID. `MIP.STIP` and
`MIP.SSIP` are not writable, so OpenSBI uses platform hooks for them
(`opensbi/patches/0005`, `0006`).

The `time` CSR is in the hardware. A read from user mode takes approximately
2 CPU cycles and does not trap. `mtime` has the same rate when the hart is
idle and when it is busy: -17 ppm and +10 ppm against a host clock, with a
resolution of approximately 30 ppm. It also counts in WFI when
`CPU_WAIT_MODE_FORCE_ON` (`0x20586118`, bit 0) is cleared.

A uniprocessor kernel still needs a self-interrupt for `irq_work`. CLIC
input 1 accepts IP writes but does not arbitrate on this revision. So the
driver uses external input 16 as a software interrupt (`patches/0005`).

## Cache and DMA

This is the part that is most different from other RISC-V SoCs.

- **No coherent bus master.** SD, USB, LCD and the GDMA do not snoop the
  D-cache.
- **No Zicbom, and Sv32 has no cacheability bits.** So Linux cannot make a
  usual mapping uncached. The hardware has two other ways. PSRAM has a
  cache-bypass alias at `0xC0000000` (the address plus `0x70000000`). It
  needs no PMA entry and works from S-mode, and the port does not use it.
  And bit 27 of a PMA cfg makes a range uncached at its usual address. The
  PMA entry with the lowest index has priority. The port uses this for the
  frame buffer. An uncached word read takes approximately 120 CPU cycles,
  and a write approximately 50.
- **Two rules for uncached memory.** A store through one address of a word
  is visible to a load through its other address only after a `fence`. And
  when a range becomes uncached, its lines must not be in the cache: the
  write-back of an old dirty line overwrites newer uncached data. The loader
  writes back and invalidates the full cache before the handoff.
- **The cache controller has a sync engine.** It writes back or invalidates
  an address range, one operation at a time, through global registers. Two
  masters that use it at the same time corrupt each other. OpenSBI programs
  it for Linux. The firmware on hart 0 also uses it, through the ROM, when
  ESP-IDF invalidates the flash window after a program or erase for Linux.
  So the two take a lock word in the shared block for each operation
  (`ESP_SHM_CACHE_LOCK_OFF`); the firmware wraps the two ROM functions with
  `--wrap`. The lock covers only the short engine operation, never the
  program or erase. Linux asks through a vendor SBI extension
  (`0x09000612`, functions: write back all, invalidate, write back,
  write back and invalidate, I-cache sync). See
  `drivers/cache/esp32s31-cache.c`. An
  erratum: a write-back can lose part of the range, so OpenSBI runs it
  twice, as the ESP-IDF ROM patch does. A test of 360 write-backs did not
  show the loss, but ESP-IDF marks it as necessary for this chip.
- **Coherent allocations come from SRAM.** The CPU accesses SRAM without
  the D-cache. The 64 KiB `shared-dma-pool` at `0x2F040000` is the default
  for `dma_alloc_coherent()`.
- **Streaming DMA uses the sync engine** through
  `riscv_noncoherent_register_cache_ops()`. `setup_arch()` enables
  non-coherent DMA early (`patches/0006`), because slab gets its alignment
  from `dma_get_cache_alignment()` before the initcalls.
- **The I-cache refills from PSRAM, not from the D-cache, and `fence.i` does
  not invalidate it.** New code (module load, `exec`, JIT) can be in a dirty
  D-cache line. The I-cache also keeps the old line until other code replaces
  it. Measured from user space: a write, a flush with `fence.i` and a call of
  the same address gave the old code in 292 of 300 runs. So
  `local_flush_icache_all()` asks OpenSBI to write the D-cache back and to
  invalidate the I-cache of the hart (`patches/0009`, vendor SBI function 4).
  For a new user code page, `flush_icache_pte()` gives only the range of the
  folio (same patch). A test that changes the code in a file page, maps it
  again and calls it gets the new code. Without a new mapping (no sync), it
  gets the old code.
- **The frame buffer is uncached.** PSRAM is write-back. The LCD DMA reads
  the frame buffer from PSRAM with no cache maintenance, so OpenSBI makes
  that range uncached with two PMA entries. Write-through for all of PSRAM
  (bit 26 of the PMA cfg) also gives a correct picture, but then Linux is 2
  to 10 times slower.

### Drivers that needed a change for this

- **SD (`dw_mmc`).** The PIO FIFO port does not work on this silicon. The
  driver must use the internal DMA (IDMAC). Its descriptor ring comes from
  the coherent DMA pool in internal SRAM, so the stock driver works.
- **USB (DWC2).** Buffer DMA works through the sync engine. Descriptor DMA
  stays off. The reset FIFO sizes are larger than the 896-word FIFO RAM, so
  the driver sets smaller ones (`patches/0004`). The DWC2 register window
  must include the FIFO windows at `0x1000 × (channel + 1)`.

## Protection

- **PMP.** OpenSBI gives its memory (`0x50F00000`, 256 KiB) no S/U access.
  PMP entries must not overlap on this CPU, even for a short time.
- **APM.** `HP_APM` checks the accesses of the bus masters, also DMA to
  PSRAM. M-mode is TEE mode, and the DMA masters are REE mode. OpenSBI
  gives its memory to TEE mode only and locks the regions. A blocked access
  returns 0 and is recorded, with no bus error. PMP does not cover the
  uncached alias of the OpenSBI memory (`0xC0F00000`). There, an S-mode
  read also returns 0 and a write is lost. A read that hits the D-cache is
  not checked: the APM sees only bus accesses, so PMP must protect the
  cached addresses. `CPU_APM` checks only CPU accesses, and `HP_MEM_APM`
  covers only SRAM.
- **The word below an APM region.** `HP_APM` puts the last word below the
  start of a region into that region. Thus `0x50EFFFFC` is TEE only: a DMA
  write to it is lost, and an S-mode cache line fill gets wrong data for
  it. The device tree reserves the page `0x50EFF000` as no-map, so Linux
  never uses it.
- **`TEE_SUPERVISOR_PRIV_SEL`** (`0x20504200`) is 0 after the boot, and
  S-mode runs with 0. When it is set for a hart, the S-mode accesses of that
  hart use the M-mode (TEE) path, and the APM protection above is gone.
- **The APM registers themselves** (`0x20504000`) are M-mode only, through
  an OpenSBI domain region.
- **Hart 0.** It runs ESP-IDF in M-mode, where only a locked PMP entry
  applies. After the loader starts hart 1, it locks hart 0 out of PSRAM,
  of the uncached PSRAM alias and of the DMA pool (PMP entries 13 to 15, no
  access). The lock stays until the next reset. Hart 0 can still write the
  rest of SRAM and all peripherals.

## Radios

The radios are ESP-Hosted (`external/esp-hosted-linux`). The loader includes
the ESP-Hosted firmware (`bootloader/components/esp_hosted/`). ESP-Hosted
supplies its own ESP32-S31 Wi-Fi libraries with the full-MAC hooks
(`esp/esp_driver/lib/esp32s31`), and the component links these in place of
the libraries of ESP-IDF. The kernel has the ESP-Hosted host driver
`esp32_shm` built in, a cfg80211 full-MAC driver. A Buildroot linux
extension (`br2-external/linux/`) copies it from the submodule into
`drivers/net/wireless/espressif/esp_hosted`, next to the shared-memory
transport (`linux/drivers/net/wireless/espressif/esp_hosted/esp_shm.c`).
The firmware runs the 802.11 MAC. `wpa_supplicant` and `hostapd` on Linux
run the authentication and the key handshakes. ESP-Hosted has no
transport for a host on the same chip, so the two sides use the frames of
the USB transport in shared SRAM. The layout is in
`shared/esp32s31-hosted-shm.h`, which the loader and the driver include.

- Two byte rings in SRAM at `0x2F050000`: 16 KiB to the firmware and 32 KiB
  to Linux. Each record is a 32-bit length and one ESP-Hosted frame. Each
  ring has one producer and one consumer, so no lock is necessary. The harts
  access SRAM without the D-cache.
- Two cross-core interrupts are the doorbells. Linux sets `FROM_CPU_1`, and
  the firmware sets `FROM_CPU_2`. `FROM_CPU_0` is not usable: the ESP-IDF
  cross-core handler on hart 0 clears it.
- A request word in the control block carries the requests to the
  firmware: a new boot event from Linux, and poweroff from OpenSBI. At
  poweroff, the firmware stops Wi-Fi and puts the chip into deep sleep
  without a wake-up source.
- A producer rings only when the consumer has read all frames, because only
  then the consumer can wait. Each side stores its index, then does a full
  fence, then loads the index of the other side. Thus one of the two sides
  always sees the other, and no frame waits for a doorbell that does not
  come. At full load, Linux gets one interrupt for approximately 5 frames.
- Linux writes a frame into the ring in `ndo_start_xmit`, and uses a worker
  only when the ring is full. When 64 frames wait, it stops the net queue.
  At full iperf3 TCP load, Linux has almost no idle time: the copy from
  SRAM, the RX worker and the network stack use the hart. The release
  notes give the throughput of each release.
- A restart of the firmware also restarts Linux. Thus when the host driver
  opens or closes the data path, the firmware only sends its boot event
  again. Over USB, close restarts the firmware. Here the firmware keeps its
  Wi-Fi state. The fatal error paths of the firmware still call
  `esp_restart()`, which restarts the full chip.
- When Wi-Fi stops, the firmware drops the events that are still in its
  event loop. The host waited 5 s for such a disconnect event and then asked
  for a firmware restart. A patch of the host driver stops that wait when
  `DEINIT` succeeds (`br2-external/package/esp-hosted-linux/0003-*`).
- The loader starts Bluetooth before hart 1, as Wi-Fi: a radio start can
  access the flash. Linux then asks for the same service
  (`esp32_shm.radio_service=bt`), which changes nothing.
- The IEEE 802.15.4 radio is not used. Its RCP took SRAM from the Wi-Fi
  buffers, and its coexistence scheme halved the Wi-Fi throughput.
- The Bluetooth controller reports "BR/EDR Not Supported" in its LMP
  features, although it has BR/EDR. A host patch clears the bit
  (`0006-*`).
- The ESP-Hosted Wi-Fi libraries need `rom.patch` of ESP-Hosted in ESP-IDF.
  Without it, the ROM handles the received EAPOL frames, and the 4-way
  handshake fails. `scripts/patch-esp-idf.sh` applies it as a local
  commit.
- With WPA3-SAE, `wpa_supplicant` on hart 1 can need more time between the
  SAE commit and the confirm than the closed Wi-Fi library on hart 0 allows
  (`cnx_auth_timeout`). Then the first connection fails, and the second try
  connects.

## The flash root without a card

The ESP32-S31-WROOM-3-N16R16V alone has no card. Linux has no driver for the
SPI flash, and it must not have one: hart 0 runs its code from the same
flash, through the cache. So hart 0 maps the flash for Linux and writes it
for Linux, through a mailbox in the shared block (`ESP_SHM_FLASH`), and
Linux sees a standard MTD device.

- **The window.** The firmware gives Linux the `rootfs` and `data`
  partitions of its partition table, which must be adjacent, and nothing
  else. `esp32s31.dts` describes the same layout as `fixed-partitions`
  under the `flash` node.
- **Reads.** At start, the firmware maps the window in the flash cache
  (`spi_flash_mmap`) and writes the address to the mailbox. Linux maps it
  write-back and reads it with `memcpy`, without hart 0: 15.5 MB/s. A read
  through the mailbox gave 3.9 MB/s, because it needed two cross-core
  interrupts and a stall for each 2 KiB.
- **Programs and erases.** Program (at most 2 KiB, in one 4 KiB sector)
  and erase (one 64 KiB block). Linux writes the address, the length, the
  data and then the command, and rings. The firmware (`shm_flash.c`) writes
  the status, clears the command and rings back. The driver
  (`esp_shm_flash.c`) sleeps until then.
- **Flash and cache.** Without flash auto-suspend, a program or erase of
  ESP-IDF freezes the cache of the two harts, and Linux runs from PSRAM
  through that cache. With `CONFIG_SPI_FLASH_AUTO_SUSPEND`, the cache stays
  on, and each cache miss to the flash suspends the operation. Linux runs
  on, and the firmware erases a 64 KiB block in one operation.
- **Bus timeouts.** A cache miss of hart 1 to PSRAM can wait behind the
  suspends longer than the core bus timeout (`HP_SYSTEM_CORE_IBUS/DBUS_TIMEOUT_CONF`,
  65535 cycles at most). The bus then gives an error, and Linux gets an
  access fault on a correct address (`CORE1_DBUS_TIMEOUT` in
  `HP_SYSTEM_CORE_TIMEOUT_INT_RAW`, seen with JTAG). The firmware turns
  both timeouts off before it starts hart 1.
- **Coherency.** After a program or erase, ESP-IDF invalidates the cache
  lines of each mapped page that it changed. The two harts share the data
  cache, so Linux reads the new data. The driver holds its lock in reads
  too, so that a read does not see half of a program.
- **The root.** A squashfs on `mtdblock0` (`rootfs`, 7.5 MiB), and UBIFS
  on `mtd1` (`data`, 2.4375 MiB) as an overlayfs over it
  (`/sbin/overlay-init`, as in OpenWrt). UBI attaches an
  erased partition and formats it. The upper layer is UBIFS and not jffs2,
  because jffs2 has no `RENAME_WHITEOUT`: overlayfs then cannot make a
  directory where a lower directory was deleted (`EINVAL`).
- **Compression.** The squashfs is zstd at the highest level that
  Buildroot gives (`BR2_TARGET_ROOTFS_SQUASHFS_EXTREME_COMP`), with 32 KiB
  blocks. gzip does not fit with BlueZ. xz is smaller, but the kernel
  decompresses it more slowly on this core. Smaller blocks decompress less
  data for each page fault.
- **The erase block.** The flash erases 4 KiB sectors, but the MTD device
  shows 64 KiB erase blocks: UBIFS needs a logical erase block of at least
  15 KiB, and UBI takes 128 bytes of each block for its headers.

## The three builds

The three builds share one Buildroot base
(`br2-external/board/esp32s31/defconfig/common.fragment`), and
`scripts/mkdefconfigs.sh` merges it with the fragment of the root and the
fragment of the board. The module alone has its root in the flash
(`flashroot.fragment`). The Korvo-1 and the Function-CoreBoard-1 have it on
a disk, the card or a USB stick (`diskroot.fragment`). A setting is in one
fragment only, and `make check` fails when a defconfig
in `br2-external/configs/` differs from its fragments.

| | Module alone | Function-CoreBoard-1 | Korvo-1 |
| --- | --- | --- | --- |
| Base | Kernel, toolchain, BusyBox, Dropbear, OpenSSL, `wpa_supplicant` (WPA3, PEAP/TTLS/TLS, AP mode), curl, iperf3, libgpiod, BlueZ (`bluetoothd`, `bluetoothctl`), Lua | The same | The same |
| Root | squashfs root in the flash, UBIFS overlay, the MTD tools | ext4 root on the USB stick | ext4 root on the card |
| Board parts | | Ethernet, USB, the LED | LCD, touch, the card, USB, the LED |
| Tools | The base only | `hostapd`, the classic BlueZ tools, bash, vim, gdb, strace, tcpdump, links, MicroPython, htop, filesystem tools, tmux, nano, nnn, ncdu, tree, bc, sqlite3, ethtool | `hostapd`, the classic BlueZ tools, bash, vim, gdb, strace, tcpdump, links, MicroPython, htop, filesystem tools, tmux, nano, nnn, ncdu, tree, bc, sqlite3, `evtest`, `gpm`, a tmux desktop on the LCD, lynx, irssi |
| Swap | zram only | zram, then p3 of the stick | zram, then p3 of the card |

The Korvo-1 and the Function-CoreBoard-1 have a disk with much space, and
the module alone has only its flash. The base keeps the network, security
and Bluetooth stack the same on all boards.

## Silicon traps

| Trap | Effect |
| --- | --- |
| `mret` from S-mode does not trap | It changes M-mode state from S-mode. |
| CLIC input not given to S-mode | S-mode window reads 0, writes are lost, no fault. |
| `sret` without the full `scause` | `SIL` stays high, all S-mode interrupts masked. |
| SD PIO FIFO | Does not work. Use IDMAC. |
| Cache write-back erratum | A single write-back can lose data. Run it twice. |
| `fence.i` | Does not invalidate the I-cache. Use the sync engine. |
| Sync engine range | It does not round the size up to a line. Align address and size. |
| `HP_APM` region start | The last word below a region belongs to the region. Keep the page below it out of use. |
| `cliccfg` layout | Not the layout in the ESP-IDF header. See "Interrupts". |
| PMP entries overlap | Fault, even for a short time during an update. |
| PMA entries overlap | No fault. The entry with the lowest index is used. |
| FPU `EXT_ILL` | Cannot identify FLW/FSW. |
| GPIO33/34 | LCD data pins and also the USB Serial/JTAG pins. |
| Flash program or erase | Freezes the cache of the two harts. Use auto-suspend. |
| Core bus timeout | Fires behind flash suspends: access fault on PSRAM. Turn it off. |
| DMA burst to PSRAM | More than 16 beats gives an MSPI error that stops hart 0. The GMAC uses PBL 16 without the x8. |

## Where to start reading

| To understand | Read |
| --- | --- |
| The handoff | `bootloader/main/main.c` |
| M-mode setup | `opensbi/platform/esp32s31/platform.c` |
| Interrupt entry | `handle_exception` in `arch/riscv/kernel/entry.S`, `linux/patches/0001` |
| Interrupt controller | `linux/drivers/irqchip/irq-esp32s31-clic.c` |
| DMA coherency | `linux/drivers/cache/esp32s31-cache.c`, `linux/patches/0006` |
| Wi-Fi transport | `shared/esp32s31-hosted-shm.h` |
| The flash root | `bootloader/components/esp_hosted/shm_flash.c`, `linux/drivers/net/wireless/espressif/esp_hosted/esp_shm_flash.c` |
| The boards | `bootloader/main/board_*.c`, `br2-external/board/esp32s31/defconfig/*.fragment` (`make defconfigs` makes `br2-external/configs/*_defconfig`) |
| Pins and GPIO | `linux/drivers/pinctrl/pinctrl-esp32s31.c` |
| Clocks and resets | `linux/drivers/clk/clk-esp32s31.c` |
| Device tree | `linux/arch/riscv/boot/dts/espressif/esp32s31.dtsi` |
