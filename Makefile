SHELL := /bin/bash

BUILD_DIR := build
LOG_DIR := logs

IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_TOOLS_PATH ?= $(HOME)/.espressif
PYTHON ?= $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/python_env/*/bin/python)))
ESPTOOL := $(PYTHON) -m esptool
ESP_RISCV_BIN := $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin)))
CROSS_COMPILE ?= $(ESP_RISCV_BIN)/riscv32-esp-elf-

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
GMAKE ?= $(shell command -v make 2>/dev/null)
GSED ?= $(shell command -v sed 2>/dev/null)
GFIND ?= $(shell command -v find 2>/dev/null)
GNU_HOST_PATH :=
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
else
BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null)
GMAKE ?= $(shell command -v gmake 2>/dev/null)
GSED := $(BREW_PREFIX)/opt/gnu-sed/libexec/gnubin/sed
GFIND := $(BREW_PREFIX)/opt/findutils/libexec/gnubin/find
GNU_HOST_PATH := $(BREW_PREFIX)/opt/gnu-sed/libexec/gnubin:$(BREW_PREFIX)/opt/findutils/libexec/gnubin
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
endif

OPENSBI_DIR := external/opensbi
OPENSBI_SRC := $(BUILD_DIR)/opensbi-src
OPENSBI_OUT := $(CURDIR)/$(BUILD_DIR)/opensbi
OPENSBI_PATCHES := $(sort $(wildcard opensbi/patches/*.patch))

LINUX_DIR := external/linux
LINUX_SRC := $(BUILD_DIR)/linux-src
LINUX_OUT := $(CURDIR)/$(BUILD_DIR)/linux
LINUX_PATCHES := $(sort $(wildcard linux/patches/*.patch))
LINUX_OVERLAY_DIRS := $(filter-out linux/patches,$(wildcard linux/*))
LINUX_HOSTCFLAGS := -I$(CURDIR)/scripts/hostshim -include $(CURDIR)/scripts/hostshim/mac-compat.h -D_UUID_T -D__GETHOSTUUID_H

BUSYBOX_DIR := external/busybox
BUSYBOX_OUT := $(CURDIR)/$(BUILD_DIR)/busybox
BUSYBOX_BIN ?= $(BUSYBOX_OUT)/busybox
BUSYBOX_CONFIG_STAMP := $(BUSYBOX_OUT)/.rootfs-config-v2
LINUX_CROSS_COMPILE ?= riscv32-linux-musl-
ZIG ?= $(shell command -v zig 2>/dev/null)
ZIGCC := $(BUSYBOX_OUT)/zigcc-rv32
LINUX_CROSS_GCC := $(shell command -v $(LINUX_CROSS_COMPILE)gcc 2>/dev/null)
BUSYBOX_CC ?= $(if $(LINUX_CROSS_GCC),$(LINUX_CROSS_GCC),$(ZIGCC))
BUSYBOX_CROSS_COMPILE ?= $(if $(LINUX_CROSS_GCC),$(LINUX_CROSS_COMPILE),$(CROSS_COMPILE))

FLASH_PORT ?=
SERIAL_PORT ?=
RESET_PORT ?=
BAUD ?= 115200
BOOT_TIMEOUT ?= 300
OPENOCD_CFG ?= openocd/esp32s31-linux.cfg

BOOTLOADER_OFFSET := 0x2000
PARTITION_TABLE_OFFSET := 0x8000
APP_OFFSET := 0x20000
OPENSBI_OFFSET := 0x220000
LINUX_OFFSET := 0x2a0000
LINUX_SIZE_OFFSET := 0xa1fff4
INITRAMFS_OFFSET := 0xa20000

.DEFAULT_GOAL := help

.PHONY: help check ports build bootloader opensbi linux busybox initramfs flash monitor openocd clean

help:
	@printf '%s\n' \
		'ESP32-S31 Linux (macOS host)' \
		'' \
		'  make check                         verify host tools and submodules' \
		'  make ports                         list connected serial devices' \
		'  make build                         build loader, OpenSBI, Linux, rootfs' \
		'  make flash FLASH_PORT=/dev/cu.X    build and flash the complete image' \
		'  make monitor SERIAL_PORT=/dev/cu.X open the BusyBox UART terminal' \
		'  make openocd                       start JTAG via GPIO33/34 USB breakout' \
		'  make clean                         remove generated build output' \
		'' \
		'BusyBox uses riscv32-linux-musl-gcc when present, otherwise Zig.' \
		'Override BUSYBOX_BIN to use an existing static RV32 BusyBox binary.'

check:
	@test -n "$(PYTHON)" -a -x "$(PYTHON)" || { echo 'missing ESP-IDF Python environment'; exit 1; }
	@test -n "$(ESP_RISCV_BIN)" -a -x "$(CROSS_COMPILE)gcc" || { echo 'missing Espressif RISC-V toolchain'; exit 1; }
	@test -n "$(GMAKE)" -a -x "$(GMAKE)" || { echo 'missing GNU make'; exit 1; }
	@test -n "$(GSED)" -a -x "$(GSED)" || { echo 'missing GNU sed'; exit 1; }
	@test -n "$(GFIND)" -a -x "$(GFIND)" || { echo 'missing GNU find'; exit 1; }
	@test -f "$(IDF_PATH)/export.sh" || { echo 'missing ESP-IDF at $(IDF_PATH)'; exit 1; }
	@test -n "$(LINUX_CROSS_GCC)" -o -n "$(ZIG)" -o -x "$(BUSYBOX_BIN)" || { echo 'missing RV32 Linux compiler (e.g., Zig)'; exit 1; }
	@"$(PYTHON)" -c 'import serial' || { echo 'missing pyserial in ESP-IDF Python environment'; exit 1; }
	@git submodule status --recursive

ports:
	@"$(PYTHON)" -m serial.tools.list_ports -v

build: bootloader opensbi linux initramfs

bootloader:
	@source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && cd bootloader && idf.py -B ../$(BUILD_DIR)/bootloader build

opensbi:
	@rm -rf "$(OPENSBI_SRC)"
	@mkdir -p "$(OPENSBI_SRC)"
	@cp -R "$(OPENSBI_DIR)"/* "$(OPENSBI_SRC)/"
	@rm -rf "$(OPENSBI_OUT)"
	@for patch_file in $(OPENSBI_PATCHES); do patch -d "$(OPENSBI_SRC)" -p1 -s < "$$patch_file"; done
	@$(GMAKE) -C "$(OPENSBI_SRC)" \
		PLATFORM_DIR="$(CURDIR)/opensbi/platform" PLATFORM=esp32s31 \
		O="$(OPENSBI_OUT)" CROSS_COMPILE="$(CROSS_COMPILE)" \
		ESP32S31_USB_SERIAL_JTAG_CONSOLE=1 \
		FW_JUMP=y FW_JUMP_ADDR=0x50000000 FW_PAYLOAD=n FW_DYNAMIC=n
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.elf" "$(BUILD_DIR)/opensbi.elf"
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.bin" "$(BUILD_DIR)/opensbi.bin"

linux:
	@rm -rf "$(LINUX_SRC)"
	@mkdir -p "$(LINUX_SRC)"
	@cp -R "$(LINUX_DIR)"/* "$(LINUX_SRC)/"
	@cp -R $(LINUX_OVERLAY_DIRS) "$(LINUX_SRC)/"
	@for patch_file in $(LINUX_PATCHES); do patch -d "$(LINUX_SRC)" -p1 -s < "$$patch_file"; done
	@PATH="$(GNU_HOST_PATH):$$PATH" $(GMAKE) -C "$(LINUX_SRC)" O="$(LINUX_OUT)" \
		ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" HOSTCFLAGS="$(LINUX_HOSTCFLAGS)" esp32s31_defconfig
	@PATH="$(GNU_HOST_PATH):$$PATH" $(GMAKE) -C "$(LINUX_SRC)" O="$(LINUX_OUT)" \
		ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" HOSTCFLAGS="$(LINUX_HOSTCFLAGS)" -j$(JOBS) Image
	@cp "$(LINUX_OUT)/arch/riscv/boot/Image" "$(BUILD_DIR)/Image"
	@"$(PYTHON)" -c 'import os, struct, zlib; p="$(BUILD_DIR)/Image"; data=open(p, "rb").read(); open("$(BUILD_DIR)/linux.size", "wb").write(struct.pack("<III", 0x455a4953, len(data), zlib.crc32(data)))'

busybox:
	@set -e; \
	if test "$(abspath $(BUSYBOX_BIN))" != "$(abspath $(BUSYBOX_OUT)/busybox)"; then \
		test -x "$(BUSYBOX_BIN)" || { \
			echo 'BUSYBOX_BIN does not name an executable RV32 BusyBox binary'; exit 1; }; \
	elif test ! -x "$(BUSYBOX_BIN)" || \
		! cmp -s rootfs/busybox.config "$(BUSYBOX_CONFIG_STAMP)"; then \
		test -n "$(LINUX_CROSS_GCC)" -o -n "$(ZIG)" || { \
			echo 'missing RV32 Linux compiler (install Zig or set LINUX_CROSS_COMPILE)'; exit 1; }; \
		mkdir -p "$(BUSYBOX_OUT)"; \
		if test -z "$(LINUX_CROSS_GCC)"; then \
			printf '%s\n' '#!/bin/sh' \
				'for arg in "$$@"; do' \
				'  shift' \
				'  case "$$arg" in' \
				'    -finline-limit=0|-falign-jumps=1|-falign-labels=1|-Wl,--warn-common|-Wl,--sort-common|-Wl,--sort-section,alignment|-Wl,--verbose|-Wl,-Map,*) continue;;' \
				'    -Wp,-MD,*) set -- "$$@" -MD -MF "$${arg#-Wp,-MD,}"; continue;;' \
				'  esac' \
				'  set -- "$$@" "$$arg"' \
				'done' \
				'exec env ZIG_GLOBAL_CACHE_DIR="$(BUSYBOX_OUT)/zig-cache" "$(ZIG)" cc -target riscv32-linux-musl -mcpu=generic_rv32+m+a+c "$$@"' \
				> "$(ZIGCC)"; \
			chmod +x "$(ZIGCC)"; \
		fi; \
		$(GMAKE) -C "$(BUSYBOX_DIR)" O="$(BUSYBOX_OUT)" \
			CROSS_COMPILE="$(BUSYBOX_CROSS_COMPILE)" CC="$(BUSYBOX_CC)" allnoconfig >/dev/null; \
		while IFS= read -r setting; do \
			name="$${setting%%=*}"; \
			"$(GSED)" -i -e "s|^# $$name is not set$$|$$setting|" \
				-e "s|^$$name=.*$$|$$setting|" "$(BUSYBOX_OUT)/.config"; \
		done < rootfs/busybox.config; \
		$(GMAKE) -C "$(BUSYBOX_DIR)" O="$(BUSYBOX_OUT)" \
			CROSS_COMPILE="$(BUSYBOX_CROSS_COMPILE)" CC="$(BUSYBOX_CC)" oldconfig >/dev/null; \
		$(GMAKE) -C "$(BUSYBOX_DIR)" O="$(BUSYBOX_OUT)" \
			CROSS_COMPILE="$(BUSYBOX_CROSS_COMPILE)" CC="$(BUSYBOX_CC)" -j$(JOBS) busybox; \
		cp rootfs/busybox.config "$(BUSYBOX_CONFIG_STAMP)"; \
	fi

initramfs: busybox
	@"$(PYTHON)" scripts/mkinitramfs.py --busybox "$(BUSYBOX_BIN)" \
		--init rootfs/init --output "$(BUILD_DIR)/initramfs.cpio" --size 0x200000

flash: build
	@test -n "$(FLASH_PORT)" || { echo 'set FLASH_PORT=/dev/cu.<flash-port>'; exit 1; }
	@$(ESPTOOL) --chip esp32s31 -p "$(FLASH_PORT)" -b 921600 \
		--before default-reset --after hard-reset write-flash \
		$(BOOTLOADER_OFFSET) "$(BUILD_DIR)/bootloader/bootloader/bootloader.bin" \
		$(PARTITION_TABLE_OFFSET) "$(BUILD_DIR)/bootloader/partition_table/partition-table.bin" \
		$(APP_OFFSET) "$(BUILD_DIR)/bootloader/s31-linux-loader.bin" \
		$(OPENSBI_OFFSET) "$(BUILD_DIR)/opensbi.bin" \
		$(LINUX_OFFSET) "$(BUILD_DIR)/Image" \
		$(LINUX_SIZE_OFFSET) "$(BUILD_DIR)/linux.size" \
		$(INITRAMFS_OFFSET) "$(BUILD_DIR)/initramfs.cpio"

monitor:
	@test -n "$(SERIAL_PORT)" || { echo 'set SERIAL_PORT=/dev/cu.<external-uart>'; exit 1; }
	@"$(PYTHON)" scripts/reset-monitor.py --port "$(SERIAL_PORT)" --baud "$(BAUD)" \
		$(if $(RESET_PORT),--reset-port "$(RESET_PORT)",) --log-dir "$(LOG_DIR)" \
		--timeout "$(BOOT_TIMEOUT)" --success-pattern 'ESP32-S31 Linux / BusyBox' --interactive

openocd:
	@mkdir -p "$(LOG_DIR)"
	@openocd_bin="$$(command -v openocd 2>/dev/null || true)"; \
	if test -z "$$openocd_bin"; then \
		openocd_bin="$$(find "$(IDF_TOOLS_PATH)/tools/openocd-esp32" \
			\( -path '*/bin/openocd' -o -path '*/bin/openocd.exe' \) \
			-type f 2>/dev/null | sort | tail -n 1)"; \
	fi; \
	test -n "$$openocd_bin" || { echo 'missing OpenOCD'; exit 1; }; \
	exec "$$openocd_bin" -c 'set ESP_RTOS none' -f "$(OPENOCD_CFG)" \
		-l "$(LOG_DIR)/$$(date +%Y%m%d-%H%M%S)-openocd.log"

clean:
	@rm -rf "$(BUILD_DIR)"
