# Copy the boot console to the USB Serial/JTAG CDC endpoint.
ESP32S31_USB_SERIAL_JTAG_CONSOLE ?= 1

# Uncached frame buffer for the LCD.  The Makefile sets it from the loader config.
ESP32S31_FB_UNCACHED ?= 0

# The shared-memory layout of the ESP-Hosted firmware, for poweroff and
# the cache lock.
platform-cppflags-y = -DFW_NO_MSTATUSH -DCLIC_NO_SIE \
	-I$(platform_src_dir)/../../../shared \
	-DESP32S31_USB_SERIAL_JTAG_CONSOLE=$(ESP32S31_USB_SERIAL_JTAG_CONSOLE) \
	-DESP32S31_FB_UNCACHED=$(ESP32S31_FB_UNCACHED)
platform-cflags-y = $(platform-cppflags-y)
platform-asflags-y = $(platform-cppflags-y)
platform-ldflags-y =

PLATFORM_RISCV_XLEN = 32
PLATFORM_RISCV_ABI = ilp32
PLATFORM_RISCV_ISA = rv32imac_zicsr_zifencei
PLATFORM_RISCV_CODE_MODEL = medany

PLATFORM_HART_COUNT = 1

# fw_jump runs from reserved PSRAM and starts the kernel in PSRAM.  Internal
# SRAM is for the ESP-IDF firmware, which stays in memory on hart 0.
FW_TEXT_START = 0x50f00000
FW_JUMP = y
FW_JUMP_ADDR = 0x50000000
FW_DYNAMIC = n
FW_PAYLOAD = n

platform-objs-y += platform.o
