# ESP32-S31 internals for Linux developers

This document tells how Linux runs on the ESP32-S31, and why the port has
its current form. It is for developers who know the RISC-V Linux kernel and
want to understand this SoC. All facts are from chip revision v0.0 on the
ESP32-S31-Korvo-1 board. Most of them were measured with JTAG, because
Espressif does not publish a technical reference manual for this chip yet.

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
| PSRAM | 16 MiB octal at `0x50000000`, cached. Linux uses it as RAM. |
| Flash | 16 MB NOR, mapped by the MMU of the flash controller |

## Boot chain

```text
ROM -> ESP-IDF 2nd stage -> loader (hart 0) -> OpenSBI (hart 1, M) -> Linux (hart 1, S)
```

1. The ESP-IDF second-stage bootloader starts the loader application
   (`bootloader/main/`) on hart 0.
2. The loader starts PSRAM, the SD slot pins, the RMT for the LED, the LCD
   and Wi-Fi. It copies three flash partitions to PSRAM and checks them:
   the kernel `Image` (size and CRC32 from a manifest at `0xA1FFF4`), the
   initramfs, and OpenSBI `fw_jump`.
3. The loader writes the D-cache back, sets the PMA of hart 1 for PSRAM and
   releases hart 1 at the OpenSBI entry.
4. Hart 0 stays in the loader. The loader is now the Wi-Fi firmware.
5. OpenSBI sets up the CLIC, the timer, PMP, APM and the SBI extensions.
   Then it jumps to the kernel in S-mode.
6. The kernel has a built-in DTB (`esp32s31_generic.dts`). The initramfs
   mounts the ext4 root from the SD card and does `switch_root`.

### Memory map

| Address | Size | Use |
| --- | --- | --- |
| `0x2F040000` | 64 KiB | Coherent DMA pool for Linux (`shared-dma-pool`) |
| `0x2F050000` | 64 KiB | Wi-Fi shared memory (rings, commands, scan table) |
| `0x2F079000` | 4 KiB | LCD DMA descriptor ring |
| `0x50000000` | 8 MiB | Kernel image and its memory |
| `0x50800000` | 2 MiB | initramfs |
| `0x50E00000` | 256 KiB | OpenSBI (reserved, no-map) |
| `0x50F40000` | 752 KiB | LCD frame buffer (reserved, no-map) |

The loader keeps its own copy of these numbers. The device tree
(`esp32s31.dtsi`) must agree with it.

## Why Linux uses only hart 1

The Wi-Fi and Bluetooth radios need the ESP-IDF libraries. These libraries
are binary, run in M-mode and use FreeRTOS. Espressif does not supply the
"full MAC" hooks that ESP-Hosted needs for this chip. So hart 0 keeps a
small ESP-IDF application that owns the radio, and Linux talks to it
through shared SRAM (see "Wi-Fi"). The price is a uniprocessor Linux.

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
  run the ESP-IDF start code, so nothing else sets `nlbits`.
- **`sret` restores the interrupt level from `scause.spil`.** The trap exit
  must write the full CLIC `scause` back before `sret`. `mret` from S-mode
  does not trap on this revision. It changes only the machine level and
  leaves `sintstatus.SIL` high, which masks all S-mode interrupts.
- **The level stack and the scheduler.** Only `sret` lowers the level. If a
  handler schedules another task while its level is still high, that task
  runs with interrupts masked. The trampoline
  (`arch/riscv/kernel/esp32s31-clic-trampoline.S`) does an `sret` to itself
  at entry, with `spil` = 0, so that all handlers run at the base level.
- **Exceptions come to the same vector.** In CLIC mode, `stvec` catches
  exceptions and interrupts. The trampoline sends exceptions to the usual
  handlers, and masks the CLIC bits out of `scause` first.

Driver: `drivers/irqchip/irq-esp32s31-clic.c`. Interrupt specifiers have
three cells: `<CLIC ID, matrix source, type>`. The driver writes the
Interrupt Matrix route itself.

### Timer and IPI

The machine timer is CLINT-like at `0x10000000` (`mtime` at `+0xBFF8`,
`mtimecmp` at `+0x4000`). A match is CLIC input 7 (M-mode). OpenSBI takes
it and sets input 5, which is the S-mode timer ID. `MIP.STIP` and
`MIP.SSIP` are not writable, so OpenSBI uses platform hooks for them
(`opensbi/patches/0005`, `0006`).

A uniprocessor kernel still needs a self-interrupt for `irq_work`. CLIC
input 1 accepts IP writes but does not arbitrate on this revision. So the
driver uses external input 16 as a software interrupt (`patches/0006`).

## Cache and DMA

This is the part that is most different from other RISC-V SoCs.

- **No coherent bus master.** SD, USB, LCD and the GDMA do not snoop the
  D-cache.
- **No Zicbom, and no uncached alias of PSRAM.** Sv32 has no cacheability
  bits either. So Linux cannot map PSRAM as uncached.
- **The cache controller has a sync engine.** It writes back or invalidates
  an address range, one operation at a time, through global registers. Two
  masters that use it at the same time corrupt each other. So only OpenSBI
  programs it, under a lock. Linux asks through a vendor SBI extension
  (`0x09000612`, functions: write back all, invalidate, write back,
  write back and invalidate). See `drivers/cache/esp32s31-cache.c`. An
  erratum: a write-back can lose part of the range, so OpenSBI runs it
  twice, as the ROM does.
- **Coherent allocations come from SRAM.** The CPU accesses SRAM without
  the D-cache. The 64 KiB `shared-dma-pool` at `0x2F040000` is the default
  for `dma_alloc_coherent()`.
- **Streaming DMA uses the sync engine** through
  `riscv_noncoherent_register_cache_ops()`. `setup_arch()` enables
  non-coherent DMA early (`patches/0007`), because slab gets its alignment
  from `dma_get_cache_alignment()` before the initcalls.
- **The I-cache refills from PSRAM, not from the D-cache.** New code (module
  load, `exec`, JIT) can be in a dirty D-cache line. So
  `local_flush_icache_all()` writes the D-cache back before `fence.i`
  (`patches/0012`).
- **Write-back or write-through.** PSRAM is write-back. With the LCD, it is
  write-through, because the LCD DMA reads the frame buffer from PSRAM with
  no cache maintenance. The PMA in OpenSBI selects the mode.

### Drivers that needed a change for this

- **SD (`dw_mmc`).** The PIO FIFO port does not work on this silicon. The
  driver must use the internal DMA (IDMAC). Its descriptor ring is usual
  cached memory, so each change of ownership needs a sync of the full ring
  (`patches/0003`, `DW_MMC_QUIRK_NONCOHERENT_DESC`).
- **USB (DWC2).** Buffer DMA works through the sync engine. Descriptor DMA
  stays off. The reset FIFO sizes are larger than the 896-word FIFO RAM, so
  the driver sets smaller ones (`patches/0005`). The DWC2 register window
  must include the FIFO windows at `0x1000 × (channel + 1)`.

## Protection

- **PMP.** OpenSBI gives its memory (`0x50E00000`, 256 KiB) no S/U access.
  PMP entries must not overlap on this CPU, even for a short time.
- **APM.** `HP_APM` checks the accesses of the bus masters, also DMA to
  PSRAM. M-mode is TEE mode, and the DMA masters are REE mode. OpenSBI
  gives its memory to TEE mode only and locks the regions. A blocked access
  returns 0 and is recorded, with no bus error. `CPU_APM` checks only CPU
  accesses, and `HP_MEM_APM` covers only SRAM.
- **The APM registers themselves** (`0x20504000`) are M-mode only, through
  an OpenSBI domain region.
- **The limit.** Hart 0 runs ESP-IDF in M-mode. Nothing can protect
  OpenSBI or Linux from hart 0.

## Wi-Fi

`drivers/net/wireless/espressif/esp32s31-wifi.c` is a cfg80211 full-MAC
driver. The ESP-IDF firmware on hart 0 runs 802.11, the association and the
key handshakes. The ABI is one header, `shared/esp32s31-wifi-ipc.h`, which
the loader and the kernel both include.

- Two rings of 16 slots with 1536 bytes, one for each direction, in SRAM at
  `0x2F050000`. Each ring has one producer and one consumer, so no lock is
  necessary. The harts access SRAM without the D-cache.
- Two cross-core interrupts are the doorbells. Linux sets `FROM_CPU_1`, and
  the firmware sets `FROM_CPU_2`. `FROM_CPU_0` is not usable: the ESP-IDF
  cross-core handler on hart 0 clears it.
- Commands (connect, disconnect, scan) and a scan table are in the same
  block. The firmware gives parsed scan data. The driver builds the RSN and
  WPA elements from it, so `wpa_supplicant` sees usual beacons.
- `wpa_supplicant` gives the key through the 4-way handshake offload (a PMK)
  or the SAE offload (the password).

## Silicon traps

| Trap | Effect |
| --- | --- |
| `mret` from S-mode does not trap | It changes M-mode state from S-mode. |
| CLIC input not given to S-mode | S-mode window reads 0, writes are lost, no fault. |
| `sret` without the full `scause` | `SIL` stays high, all S-mode interrupts masked. |
| SD PIO FIFO | Does not work. Use IDMAC. |
| Cache write-back erratum | A single write-back can lose data. Run it twice. |
| PMP entries overlap | Fault, even for a short time during an update. |
| FPU `EXT_ILL` | Cannot identify FLW/FSW. |
| GPIO33/34 | LCD data pins and also the USB Serial/JTAG pins. |

## Where to start reading

| To understand | Read |
| --- | --- |
| The handoff | `bootloader/main/main.c` |
| M-mode setup | `opensbi/platform/esp32s31/platform.c` |
| Interrupt entry | `linux/arch/riscv/kernel/esp32s31-clic-trampoline.S` |
| Interrupt controller | `linux/drivers/irqchip/irq-esp32s31-clic.c` |
| DMA coherency | `linux/drivers/cache/esp32s31-cache.c`, `linux/patches/0007` |
| Wi-Fi ABI | `shared/esp32s31-wifi-ipc.h` |
| Device tree | `linux/arch/riscv/boot/dts/espressif/esp32s31.dtsi` |
