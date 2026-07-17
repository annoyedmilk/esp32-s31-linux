# Mirror the boot console on the native USB Serial/JTAG CDC endpoint.
ESP32S31_USB_SERIAL_JTAG_CONSOLE ?= 1

platform-cppflags-y = -DFW_NO_MSTATUSH -DCLIC_NO_SIE -DESP32S31_USB_SERIAL_JTAG_CONSOLE=$(ESP32S31_USB_SERIAL_JTAG_CONSOLE)
platform-cflags-y = $(platform-cppflags-y)
platform-asflags-y = $(platform-cppflags-y)
platform-ldflags-y =

PLATFORM_RISCV_XLEN = 32
PLATFORM_RISCV_ABI = ilp32
PLATFORM_RISCV_ISA = rv32imac_zicsr_zifencei
PLATFORM_RISCV_CODE_MODEL = medany

PLATFORM_HART_COUNT = 1

# fw_jump runs from SRAM at 0x2f000000 and enters the kernel in PSRAM.
FW_TEXT_START = 0x2f000000
FW_JUMP = y
FW_JUMP_ADDR = 0x50000000
FW_DYNAMIC = n
FW_PAYLOAD = n

platform-objs-y += platform.o
