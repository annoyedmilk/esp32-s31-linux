SHELL := /bin/bash

BUILD_DIR := build
LOG_DIR := logs

IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_TOOLS_PATH ?= $(HOME)/.espressif
PYTHON ?= $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/python_env/*/bin/python)))
ESPTOOL := $(PYTHON) -m esptool
ESP_RISCV_BIN := $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin)))
CROSS_COMPILE ?= $(ESP_RISCV_BIN)/riscv32-esp-elf-

BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null)
GMAKE ?= $(shell command -v gmake 2>/dev/null)
GSED := $(BREW_PREFIX)/opt/gnu-sed/libexec/gnubin/sed
GFIND := $(BREW_PREFIX)/opt/findutils/libexec/gnubin/find
GNU_HOST_PATH := $(BREW_PREFIX)/opt/gnu-sed/libexec/gnubin:$(BREW_PREFIX)/opt/findutils/libexec/gnubin
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

OPENSBI_DIR := external/opensbi
OPENSBI_SRC := $(BUILD_DIR)/opensbi-src
OPENSBI_OUT := $(CURDIR)/$(BUILD_DIR)/opensbi
OPENSBI_PATCHES := $(sort $(wildcard opensbi/patches/*.patch))

LINUX_DIR := external/linux
LINUX_SRC := $(BUILD_DIR)/linux-src
LINUX_OUT := $(CURDIR)/$(BUILD_DIR)/linux
LINUX_PATCHES := $(sort $(wildcard linux/patches/*.patch))
LINUX_OVERLAY_DIRS := $(filter-out linux/patches,$(wildcard linux/*))
LINUX_PREP_STAMP := $(LINUX_SRC)/.patched
LINUX_HOSTCFLAGS := -I$(CURDIR)/scripts/hostshim -include $(CURDIR)/scripts/hostshim/mac-compat.h -D_UUID_T -D__GETHOSTUUID_H

# Buildroot cannot build on macOS, so it runs in a container.  Its output/ and
# dl/ stay in a volume; several gigabytes have no business on virtiofs.
BR_DIR := external/buildroot
BR_IMAGE ?= esp32s31-buildroot:bookworm
BR_VOLUME ?= esp32s31-br
BR_VOLUME_SIZE ?= 60G
BR_DEFCONFIG := esp32s31_defconfig
BR_MEMORY ?= 8G
BR_OUT := $(BUILD_DIR)/buildroot
BR_MAKE := make O=/br/output BR2_EXTERNAL=/work/br2-external BR2_DL_DIR=/br/dl
CONTAINER ?= $(shell command -v container 2>/dev/null)
BR_RUN = "$(CONTAINER)" run --rm --cpus $(JOBS) --memory $(BR_MEMORY) \
	--uid $(shell id -u) --gid $(shell id -g) -e HOME=/br/home \
	-v $(BR_VOLUME):/br -v "$(CURDIR)":/work "$(BR_IMAGE)"

INITRAMFS_INIT := br2-external/board/esp32s31/init
SD_DISK ?=
SD_RAW = $(subst /dev/disk,/dev/rdisk,$(SD_DISK))
# FAT area; the rest of the card becomes the root partition.  Shrink this
# for a smaller card -- diskutil fails loudly if it does not fit.
SD_DATA_SIZE ?= 28G

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

.PHONY: help check ports build bootloader opensbi linux container-image \
	br-volume rootfs rootfs-menuconfig initramfs sdcard sdpart sdwrite sdroot \
	flash monitor openocd clean

help:
	@printf '%s\n' \
		'ESP32-S31 Linux (macOS host)' \
		'' \
		'  make check                         verify host tools and submodules' \
		'  make ports                         list connected serial devices' \
		'  make build                         build loader, OpenSBI, Linux, rootfs' \
		'  make flash FLASH_PORT=/dev/cu.X    build and flash the complete image' \
		'  make monitor SERIAL_PORT=/dev/cu.X open the Linux UART terminal' \
		'  make openocd                       start JTAG via GPIO33/34 USB breakout' \
		'  make clean                         remove generated build output' \
		'' \
		'Component targets, each buildable on its own:' \
		'' \
		'  make bootloader                    ESP-IDF loader application' \
		'  make opensbi                       patched OpenSBI fw_jump' \
		'  make linux                         patched kernel Image and manifest' \
		'  make rootfs                        Buildroot RV32 userspace (ext4 image)' \
		'  make initramfs                     the flash slot'\''s early userspace' \
		'' \
		'SD card, once a card is in the Mac (see: diskutil list external):' \
		'' \
		'  make sdcard                        card image for a fresh card' \
		'  make sdpart  SD_DISK=/dev/diskN   lay out a large card (ERASES it)' \
		'  make sdwrite SD_DISK=/dev/diskN   write the whole card image (ERASES it)' \
		'  make sdroot  SD_DISK=/dev/diskN   rewrite only the root partition' \
		'' \
		'Buildroot host container (it cannot build on macOS):' \
		'' \
		'  make container-image               build the Debian build image' \
		'  make rootfs-menuconfig             explore the Buildroot configuration' \
		'' \
		'FLASH_PORT is the esptool target: the native USB Serial/JTAG' \
		'(/dev/cu.usbmodem*) or the CP2102N UART bridge (/dev/cu.usbserial-*).' \
		'SERIAL_PORT is the external UART carrying the Linux console.' \
		'RESET_PORT optionally names a second port whose RTS pulse resets the' \
		'board before monitoring when SERIAL_PORT cannot (BAUD, BOOT_TIMEOUT' \
		'tune the monitor).' \
		'' \
		'The rootfs lives on the SD card; the flash slot carries only the' \
		'initramfs that switch_roots into it.'

check:
	@test "$$(uname -s)" = Darwin || { echo 'this build is supported on macOS only'; exit 1; }
	@test -n "$(BREW_PREFIX)" || { echo 'missing Homebrew'; exit 1; }
	@test -n "$(PYTHON)" -a -x "$(PYTHON)" || { echo 'missing ESP-IDF Python environment'; exit 1; }
	@test -n "$(ESP_RISCV_BIN)" -a -x "$(CROSS_COMPILE)gcc" || { echo 'missing Espressif RISC-V toolchain'; exit 1; }
	@test -n "$(GMAKE)" -a -x "$(GMAKE)" || { echo 'missing GNU make'; exit 1; }
	@test -n "$(GSED)" -a -x "$(GSED)" || { echo 'missing GNU sed'; exit 1; }
	@test -n "$(GFIND)" -a -x "$(GFIND)" || { echo 'missing GNU find'; exit 1; }
	@test -f "$(IDF_PATH)/export.sh" || { echo 'missing ESP-IDF at $(IDF_PATH)'; exit 1; }
	@test -n "$(CONTAINER)" -a -x "$(CONTAINER)" || { echo 'missing the container CLI (Buildroot cannot build on macOS)'; exit 1; }
	@"$(CONTAINER)" system status >/dev/null 2>&1 || { echo 'container services are not running: container system start'; exit 1; }
	@"$(PYTHON)" -c 'import serial' || { echo 'missing pyserial in ESP-IDF Python environment'; exit 1; }
	@git submodule status --recursive

ports:
	@"$(PYTHON)" -m serial.tools.list_ports -v

build: bootloader opensbi linux rootfs initramfs

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
		O="$(OPENSBI_OUT)" CROSS_COMPILE="$(CROSS_COMPILE)"
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.elf" "$(BUILD_DIR)/opensbi.elf"
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.bin" "$(BUILD_DIR)/opensbi.bin"

# The patched kernel tree is rebuilt only when the patch series changes.
# Overlay files (drivers, dts, defconfig) are copied fresh on every build;
# run "make clean" after updating the external/linux submodule itself.
$(LINUX_PREP_STAMP): $(LINUX_PATCHES)
	@rm -rf "$(LINUX_SRC)"
	@mkdir -p "$(LINUX_SRC)"
	@cp -R "$(LINUX_DIR)"/* "$(LINUX_SRC)/"
	@for patch_file in $(LINUX_PATCHES); do patch -d "$(LINUX_SRC)" -p1 -s < "$$patch_file"; done
	@touch "$@"

linux: $(LINUX_PREP_STAMP)
	@cp -R $(LINUX_OVERLAY_DIRS) "$(LINUX_SRC)/"
	@cp shared/esp32s31-wifi-ipc.h "$(LINUX_SRC)/drivers/net/wireless/espressif/"
	@PATH="$(GNU_HOST_PATH):$$PATH" $(GMAKE) -C "$(LINUX_SRC)" O="$(LINUX_OUT)" \
		ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" HOSTCFLAGS="$(LINUX_HOSTCFLAGS)" esp32s31_defconfig
	@PATH="$(GNU_HOST_PATH):$$PATH" $(GMAKE) -C "$(LINUX_SRC)" O="$(LINUX_OUT)" \
		ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" HOSTCFLAGS="$(LINUX_HOSTCFLAGS)" -j$(JOBS) Image
	@cp "$(LINUX_OUT)/arch/riscv/boot/Image" "$(BUILD_DIR)/Image"
	@"$(PYTHON)" -c 'import struct, zlib; p="$(BUILD_DIR)/Image"; data=open(p, "rb").read(); open("$(BUILD_DIR)/linux.size", "wb").write(struct.pack("<III", 0x455a4953, len(data), zlib.crc32(data)))'

container-image:
	@test -n "$(CONTAINER)" || { echo 'missing the container CLI'; exit 1; }
	@"$(CONTAINER)" build -t "$(BR_IMAGE)" container

# Buildroot must not run as root, so the volume is handed to the caller once.
br-volume:
	@test -n "$(CONTAINER)" || { echo 'missing the container CLI'; exit 1; }
	@"$(CONTAINER)" volume inspect "$(BR_VOLUME)" >/dev/null 2>&1 || { \
		echo "creating the $(BR_VOLUME) volume ($(BR_VOLUME_SIZE))"; \
		"$(CONTAINER)" volume create -s $(BR_VOLUME_SIZE) "$(BR_VOLUME)"; \
		"$(CONTAINER)" run --rm --uid 0 --gid 0 -v $(BR_VOLUME):/br "$(BR_IMAGE)" \
			chown -R $(shell id -u):$(shell id -g) /br; }

# The checked-in defconfig is the source of truth and is reapplied every build.
rootfs: br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		$(BR_MAKE) $(BR_DEFCONFIG); \
		$(BR_MAKE); \
		mkdir -p /work/$(BR_OUT); \
		cp /br/output/images/rootfs.ext2 /work/$(BR_OUT)/; \
		if test -f /br/output/images/sdcard.img; then \
			cp /br/output/images/sdcard.img /work/$(BR_OUT)/; fi'

rootfs-menuconfig: br-volume
	@$(BR_RUN) -i -t sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) $(BR_DEFCONFIG) && $(BR_MAKE) menuconfig'

# Runs in the container because the target tree lives in the volume.
initramfs: rootfs
	@$(BR_RUN) sh -c 'cd /work && python3 scripts/mkinitramfs.py \
		--target /br/output/target --init "$(INITRAMFS_INIT)" \
		--output "$(BUILD_DIR)/initramfs.cpio" --size 0x200000'

sdcard: rootfs
	@test -f "$(BR_OUT)/sdcard.img" || { \
		echo 'Buildroot produced no sdcard.img; check post-image.sh'; exit 1; }
	@ls -l "$(BR_OUT)/sdcard.img"

# Lay out a large card: most of it FAT for the Mac, a small ext4 root.  Use
# this instead of sdwrite when the card is bigger than the image.
sdpart:
	@test -n "$(SD_DISK)" || { echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
	@diskutil info "$(SD_DISK)" | grep -E 'Device Node|Media Name|Disk Size|Removable Media'
	@echo
	@echo "This ERASES $(SD_DISK) and everything on it."
	@read -r -p 'Type ERASE to continue: ' reply; test "$$reply" = ERASE || { echo aborted; exit 1; }
	@diskutil unmountDisk "$(SD_DISK)"
	@diskutil partitionDisk "$(SD_DISK)" MBR \
		"MS-DOS FAT32" KORVO_SD $(SD_DATA_SIZE) "MS-DOS FAT32" ROOTFS R
	@diskutil unmountDisk "$(SD_DISK)"
# Linux creates the node whatever the type byte says, but 0x83 stops macOS
# trying to mount an ext4 partition as FAT on every insert.
	@sudo sh -c 'dd if="$(SD_RAW)" of="$(BUILD_DIR)/mbr.bin" bs=512 count=1 && \
		printf "\\x83" | dd of="$(BUILD_DIR)/mbr.bin" bs=1 seek=466 conv=notrunc && \
		dd if="$(BUILD_DIR)/mbr.bin" of="$(SD_RAW)" bs=512 count=1' \
		|| echo 'warning: p2 type byte unchanged; the board boots either way'
	@rm -f "$(BUILD_DIR)/mbr.bin"
	@diskutil list "$(SD_DISK)"
	@echo 'now: make sdroot SD_DISK=$(SD_DISK)'

# Erases the card, and drops everything past the last partition on a big one.
sdwrite:
	@test -n "$(SD_DISK)" || { echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
	@test -f "$(BR_OUT)/sdcard.img" || { echo 'no $(BR_OUT)/sdcard.img; run make sdcard'; exit 1; }
	@diskutil info "$(SD_DISK)" | grep -E 'Device Node|Media Name|Disk Size|Removable Media'
	@echo
	@echo "This ERASES $(SD_DISK) and everything on it."
	@read -r -p 'Type ERASE to continue: ' reply; test "$$reply" = ERASE || { echo aborted; exit 1; }
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo dd if="$(BR_OUT)/sdcard.img" of="$(SD_RAW)" bs=4m
	@sync
	@diskutil eject "$(SD_DISK)"

# The day-to-day loop: leaves the data partition and the card's size alone.
sdroot:
	@test -n "$(SD_DISK)" || { echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
	@test -f "$(BR_OUT)/rootfs.ext2" || { echo 'no $(BR_OUT)/rootfs.ext2; run make rootfs'; exit 1; }
	@test -e "$(SD_DISK)s2" || { \
		echo '$(SD_DISK)s2 does not exist; run make sdpart first'; exit 1; }
	@echo "This overwrites the root partition $(SD_DISK)s2."
	@read -r -p 'Type WRITE to continue: ' reply; test "$$reply" = WRITE || { echo aborted; exit 1; }
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo dd if="$(BR_OUT)/rootfs.ext2" of="$(SD_RAW)s2" bs=4m
	@sync
	@diskutil eject "$(SD_DISK)"

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
		--timeout "$(BOOT_TIMEOUT)" --success-pattern 'ESP32-S31 Linux / Buildroot' --interactive

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

# Buildroot's output is in the volume: "container volume rm $(BR_VOLUME)".
clean:
	@rm -rf "$(BUILD_DIR)"
