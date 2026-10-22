SHELL := /bin/bash

# The board to build for: esp32s31 (the ESP32-S31-WROOM-3-N16R16V alone:
# UART0, root in the flash), coreboard1 (ESP32-S31-Function-CoreBoard-1:
# Ethernet, USB, LED, root on a USB stick) or korvo1 (ESP32-S31-Korvo-1: LCD,
# microSD, USB, LED, root on the card).
BOARD ?= esp32s31
ifeq ($(filter $(BOARD),esp32s31 coreboard1 korvo1),)
$(error BOARD must be esp32s31, coreboard1 or korvo1, not "$(BOARD)")
endif

BUILD_DIR := build/$(BOARD)
LOG_DIR := logs

IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_TOOLS_PATH ?= $(HOME)/.espressif
IDF_VERSION := $(shell sed -n 's/^set(IDF_VERSION_M[AI][JN]OR \([0-9]*\))/\1/p' \
	"$(IDF_PATH)/tools/cmake/version.cmake" 2>/dev/null | paste -sd. -)
PYTHON ?= $(lastword $(foreach p,$(sort $(wildcard $(IDF_TOOLS_PATH)/python_env/idf$(IDF_VERSION)_*/bin/python)),$(if $(realpath $(p)),$(p))))
ESPTOOL := $(PYTHON) -m esptool
# The tested esptool.  Its flasher stub for the chip comes with it, so a new
# esptool can change how the board is flashed.  Move it with the ESP-IDF pin.
ESPTOOL_VERSION := $(shell cat bootloader/esptool.version)
ESPTOOL_CHECK = @v=$$($(ESPTOOL) version 2>/dev/null | tail -1); \
	test "$$v" = "$(ESPTOOL_VERSION)" || { \
	echo "esptool is $$v, but bootloader/esptool.version wants $(ESPTOOL_VERSION):"; \
	echo '  $(PYTHON) -m pip install esptool==$(ESPTOOL_VERSION)'; exit 1; }
# export.sh names its virtualenv after the python3 that it finds.  After a
# host Python upgrade, that virtualenv does not exist, and the old one can
# have a broken python link.  PYTHON is the newest one that works, for the
# ESP-IDF version at IDF_PATH.
IDF_PYTHON_ENV := $(patsubst %/bin/python,%,$(PYTHON))
ESP_RISCV_BIN := $(lastword $(sort $(wildcard $(IDF_TOOLS_PATH)/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin)))
CROSS_COMPILE ?= $(ESP_RISCV_BIN)/riscv32-esp-elf-

# macOS has GNU make as gmake.  On Linux, make is GNU make.
GMAKE ?= $(shell command -v gmake 2>/dev/null || command -v make)
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

OPENSBI_DIR := external/opensbi
# The ESP-Hosted firmware needs these patches in ESP-IDF (scripts/patch-esp-idf.sh).
HOSTED_IDF_PATCHES := $(addprefix $(CURDIR)/external/esp-hosted-linux/esp/esp_driver/lib/, \
	rom.patch)
OPENSBI_SRC := $(BUILD_DIR)/opensbi-src
OPENSBI_OUT := $(CURDIR)/$(BUILD_DIR)/opensbi
OPENSBI_PATCHES := $(sort $(wildcard opensbi/patches/*.patch))
# The loader config of the board: the common defaults, then the board.
SDKCONFIG_DEFAULTS := sdkconfig.defaults;sdkconfig.defaults.$(BOARD)
# The LCD DMA reads PSRAM without the cache, so the frame buffer must not be
# cached when the loader starts the LCD.
FB_UNCACHED := $(shell grep -q '^CONFIG_ESP_CONSOLE_SECONDARY_NONE=y' \
	bootloader/sdkconfig.defaults.$(BOARD) && echo 1 || echo 0)
# The LCD has the pins of the USB Serial/JTAG, so OpenSBI copies its console
# to USB only when the loader does not start the LCD.
USB_CONSOLE := $(if $(filter 1,$(FB_UNCACHED)),0,1)

# Buildroot cannot build on macOS, so it runs in a container.  Its output
# directory for each board and dl/ are in a volume, because virtiofs is slow
# for many gigabytes.
BR_DIR := external/buildroot
BR_IMAGE ?= esp32s31-buildroot:bookworm
BR_VOLUME ?= esp32s31-br
BR_VOLUME_SIZE ?= 60G
BR_DEFCONFIG := $(BOARD)_defconfig
# The kernel release that Buildroot builds and that kernel-check uses.
LINUX_VERSION := $(shell sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="\(.*\)"/\1/p' \
	br2-external/configs/$(BR_DEFCONFIG))
# The guest page cache grows to the full limit.  With 8G on an 8 GB Mac,
# macOS swapped during a build.
BR_MEMORY ?= 6G
BR_OUT := $(BUILD_DIR)/buildroot
# GCC needs approximately 1 GB for each of its largest files.  With 6G and
# the default of one job more than the CPUs, the kernel killed cc1plus.
BR_JOBS ?= 5
BR_O := /br/$(BOARD)
BR_MAKE := make O=$(BR_O) BR2_EXTERNAL=/work/br2-external BR2_DL_DIR=/br/dl \
	BR2_JLEVEL=$(BR_JOBS)
CONTAINER ?= $(shell command -v container 2>/dev/null)
BR_RUN = "$(CONTAINER)" run --rm --cpus $(JOBS) --memory $(BR_MEMORY) \
	--uid $(shell id -u) --gid $(shell id -g) -e HOME=/br/home \
	-v $(BR_VOLUME):/br -v "$(CURDIR)":/work "$(BR_IMAGE)"
# The same image without the build volume, for checks that only read files.
BR_TOOLS = "$(CONTAINER)" run --rm --uid $(shell id -u) --gid $(shell id -g) \
	-v "$(CURDIR)":/work "$(BR_IMAGE)"

# The flash layout of the board, from its partition table: field 4 is the
# offset, field 5 the size.
PARTITIONS := bootloader/partitions-$(BOARD).csv
part_field = $(shell awk -F'[ ,]+' '$$1 == "$(1)" { print $$$(2) }' $(PARTITIONS))

# The squashfs root in the flash, on the module alone.  The Korvo-1 and the
# Function-CoreBoard-1 have their root on a disk and no rootfs partition.
ROOT_OFFSET := $(call part_field,rootfs,4)
ROOT_SIZE := $(call part_field,rootfs,5)
SD_DISK ?=
SD_RAW = $(subst /dev/disk,/dev/rdisk,$(SD_DISK))
# Size of the FAT partition.  The root partition gets the rest of the card,
# except 64 MB of swap at the end.
# Make it smaller for a small card, but not below 4G.  diskutil stops if it
# does not fit.
SD_DATA_SIZE ?= 28G

FLASH_PORT ?=
SERIAL_PORT ?=
RESET_PORT ?=
BAUD ?= 115200
BOOT_TIMEOUT ?= 300
OPENOCD_CFG ?= openocd/esp32s31-linux.cfg

BOOTLOADER_OFFSET := 0x2000
PARTITION_TABLE_OFFSET := 0x8000
APP_OFFSET := $(call part_field,factory,4)
OPENSBI_OFFSET := $(call part_field,opensbi,4)
LINUX_OFFSET := $(call part_field,linux,4)
LINUX_SIZE := $(call part_field,linux,5)
# The size manifest is at the end of the kernel partition.
LINUX_SIZE_OFFSET := $(shell printf '0x%x' $$(( $(LINUX_OFFSET) + $(LINUX_SIZE) - 12 )))

.DEFAULT_GOAL := help

.PHONY: help check ports build bootloader opensbi kernel defconfigs \
	kernel-check kernel-clean kernel-vmlinux kernel-menuconfig \
	kernel-saveconfig \
	container-image \
	br-volume br-artifacts rootfs rootfs-menuconfig sdcard sdpart \
	sdwrite sdroot flash flash-args monitor openocd clean

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
		'  make rootfs                        Buildroot: kernel, userspace, images' \
		'' \
		'Kernel, which Buildroot builds from a stock release:' \
		'' \
		'  make kernel                        rebuild just the kernel' \
		'  make defconfigs                    regenerate br2-external/configs from the fragments' \
		'  make kernel-check                  dry-run the series on a pristine tree' \
		'  make kernel-clean                  re-extract the kernel after a patch change' \
		'  make kernel-vmlinux                fetch vmlinux from the build volume for GDB' \
		'  make kernel-menuconfig             configure the kernel' \
		'  make kernel-saveconfig             write the configuration back' \
		'' \
		'Disk (the card or the USB stick), once it is in the Mac' \
		'(see: diskutil list external):' \
		'' \
		'  make sdcard                        disk image for a fresh disk' \
		'  make sdpart  SD_DISK=/dev/diskN   lay out a large disk (ERASES it)' \
		'  make sdwrite SD_DISK=/dev/diskN   write the whole disk image (ERASES it)' \
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
		'BOARD selects the board, for each target: esp32s31 (the default),' \
		'coreboard1 or korvo1.  The output of a board goes to build/BOARD.' \
		'On esp32s31 the rootfs is in the flash.  On korvo1 it is on the SD' \
		'card, on coreboard1 on a USB stick.'

check:
	@test "$$(uname -s)" = Darwin || { echo 'this build is supported on macOS only'; exit 1; }
	@test -n "$(PYTHON)" -a -x "$(PYTHON)" || { echo 'missing ESP-IDF Python environment'; exit 1; }
	@test -n "$(ESP_RISCV_BIN)" -a -x "$(CROSS_COMPILE)gcc" || { echo 'missing Espressif RISC-V toolchain'; exit 1; }
	@test -n "$(GMAKE)" -a -x "$(GMAKE)" || { echo 'missing GNU make'; exit 1; }
	@test -f "$(IDF_PATH)/export.sh" || { echo 'missing ESP-IDF at $(IDF_PATH)'; exit 1; }
	@test -n "$(CONTAINER)" -a -x "$(CONTAINER)" || { echo 'missing the container CLI (Buildroot cannot build on macOS)'; exit 1; }
	@"$(CONTAINER)" system status >/dev/null 2>&1 || { echo 'container services are not running: container system start'; exit 1; }
	@"$(CONTAINER)" image inspect "$(BR_IMAGE)" >/dev/null 2>&1 || { echo 'missing the $(BR_IMAGE) image: make container-image'; exit 1; }
	@"$(PYTHON)" -c 'import serial' || { echo 'missing pyserial in ESP-IDF Python environment'; exit 1; }
	$(ESPTOOL_CHECK)
	@$(BR_TOOLS) sh -c 'cd /work && scripts/mkdefconfigs.sh --check'
	@git submodule status --recursive

ports:
	@"$(PYTHON)" -m serial.tools.list_ports -v

build: bootloader opensbi rootfs

# idf.py reads sdkconfig.defaults only when it creates sdkconfig.  Then it
# writes sdkconfig at each build, so it ignores changes to the defaults.
# Here the defaults have priority, also over "idf.py menuconfig".
bootloader:
	@for f in bootloader/sdkconfig.defaults bootloader/sdkconfig.defaults.$(BOARD); do \
		if test -f $(BUILD_DIR)/sdkconfig && test $$f -nt $(BUILD_DIR)/sdkconfig; then \
			echo "$$f changed; regenerating $(BUILD_DIR)/sdkconfig"; \
			rm -f $(BUILD_DIR)/sdkconfig; \
		fi; \
	done
	@mkdir -p "$(BUILD_DIR)"
	@for p in $(HOSTED_IDF_PATCHES); do \
		git -C "$(IDF_PATH)" apply --recount --reverse --check "$$p" 2>/dev/null || exit 1; \
	done || { \
		echo 'apply the ESP-Hosted patches to ESP-IDF first:'; \
		echo '  IDF_PATH=$(IDF_PATH) scripts/patch-esp-idf.sh'; exit 1; }
	@export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV)"; \
	if ! source "$(IDF_PATH)/export.sh" >"$(BUILD_DIR)/idf-export.log" 2>&1; then \
		cat "$(BUILD_DIR)/idf-export.log"; \
		echo 'ESP-IDF environment setup failed (run $(IDF_PATH)/install.sh)'; \
		exit 1; \
	fi; \
	cd bootloader && idf.py -B ../$(BUILD_DIR)/bootloader \
		-D SDKCONFIG="$(CURDIR)/$(BUILD_DIR)/sdkconfig" \
		-D SDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" build

opensbi:
	@rm -rf "$(OPENSBI_SRC)"
	@mkdir -p "$(OPENSBI_SRC)"
	@cp -R "$(OPENSBI_DIR)"/* "$(OPENSBI_SRC)/"
	@rm -rf "$(OPENSBI_OUT)"
	@# A for loop returns the status of its last iteration.  Without the exit,
	@# a failed patch before the last one gives an unpatched OpenSBI.
	@for patch_file in $(OPENSBI_PATCHES); do \
		patch -d "$(OPENSBI_SRC)" -p1 -F0 -s < "$$patch_file" || \
			{ echo "$$patch_file does not apply"; exit 1; }; \
	done
	@$(GMAKE) -C "$(OPENSBI_SRC)" \
		PLATFORM_DIR="$(CURDIR)/opensbi/platform" PLATFORM=esp32s31 \
		O="$(OPENSBI_OUT)" CROSS_COMPILE="$(CROSS_COMPILE)" \
		ESP32S31_FB_UNCACHED=$(FB_UNCACHED) \
		ESP32S31_USB_SERIAL_JTAG_CONSOLE=$(USB_CONSOLE)
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.elf" "$(BUILD_DIR)/opensbi.elf"
	@cp "$(OPENSBI_OUT)/platform/esp32s31/firmware/fw_jump.bin" "$(BUILD_DIR)/opensbi.bin"

# The defconfigs are made from the common fragment, the root fragment and
# the fragment of each board.
defconfigs:
	@$(BR_TOOLS) sh -c 'cd /work && scripts/mkdefconfigs.sh'

# Apply the series in order to a clean extract of the kernel release, in the
# container at zero fuzz, as Buildroot does.  The macOS patch accepts old
# context that GNU patch rejects.  A kernel version change must also pass
# this test.
kernel-check: br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		test -f /br/dl/linux/linux-$(LINUX_VERSION).tar.xz || { \
			$(BR_MAKE) $(BR_DEFCONFIG) >/dev/null; \
			$(BR_MAKE) linux-source >/dev/null; }; \
		rm -rf /br/check && mkdir -p /br/check; \
		tar -xf /br/dl/linux/linux-$(LINUX_VERSION).tar.xz \
			--strip-components=1 -C /br/check; \
		cd /br/check; fail=0; \
		for p in /work/linux/patches/*.patch; do \
			printf "%-50s " "$$(basename $$p)"; \
			if patch -p1 -F0 -s -f < "$$p" >/dev/null 2>&1; \
				then echo applies; else echo FAILS; fail=1; fi; \
		done; rm -rf /br/check; exit $$fail'

# Buildroot copies linux/ and applies the patches only when it extracts the
# kernel, and it ignores changes to the custom config and to the ESP-Hosted
# sources.  Thus keep a hash of all of them in the volume.  When the hash changes, delete the
# extracted kernel, headers and ESP-Hosted sources, so Buildroot extracts,
# patches and configures them again.  Runs in the container, in the Buildroot
# directory.
KERNEL_SYNC = sum=$$({ find /work/linux -type f ! -name ._\* | sort | xargs cat; \
		cat /work/br2-external/board/esp32s31/linux* \
		/work/br2-external/configs/$(BR_DEFCONFIG) \
		/work/shared/*.h /work/br2-external/package/esp-hosted-linux/* \
		/work/external/esp-hosted-linux/host/*.c \
		/work/external/esp-hosted-linux/host/include/*.h; } | \
		sha256sum | cut -d" " -f1); \
	stamp=$(BR_O)/.esp32s31-kernel-inputs; \
	if test -f $(BR_O)/.config && \
		test "$$(cat $$stamp 2>/dev/null)" != "$$sum"; then \
		echo "kernel sources or config changed: extracting the kernel again"; \
		$(BR_MAKE) linux-dirclean linux-headers-dirclean \
			esp-hosted-linux-dirclean; \
	fi; \
	mkdir -p $(BR_O); echo "$$sum" > $$stamp

kernel: br-volume
	@$(BR_RUN) sh -c 'set -e; cd /work/$(BR_DIR); $(KERNEL_SYNC); \
		$(BR_MAKE) linux-rebuild all'
	@$(MAKE) --no-print-directory br-artifacts

# Delete the extracted kernel and headers by hand.  "make kernel" and
# "make rootfs" do this themselves when linux/, the config or the
# ESP-Hosted sources change.
kernel-clean: br-volume
	@$(BR_RUN) sh -c 'cd /work/$(BR_DIR); $(BR_MAKE) linux-dirclean linux-headers-dirclean'

# vmlinux has 100 MB of DWARF, so copy it only when GDB needs it.
kernel-vmlinux: br-volume
	@$(BR_RUN) sh -c 'cp $(BR_O)/build/linux-$(LINUX_VERSION)/vmlinux /work/$(BUILD_DIR)/'
	@ls -l "$(BUILD_DIR)/vmlinux"

kernel-menuconfig: br-volume
	@$(BR_RUN) -i -t sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) linux-menuconfig'

kernel-saveconfig: br-volume
	@$(BR_RUN) sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) linux-update-defconfig'
	@git diff --stat -- br2-external/board/esp32s31/linux.config

container-image:
	@test -n "$(CONTAINER)" || { echo 'missing the container CLI'; exit 1; }
	@"$(CONTAINER)" build -t "$(BR_IMAGE)" container

# Buildroot must not run as root, so give the volume to the user one time.
br-volume:
	@test -n "$(CONTAINER)" || { echo 'missing the container CLI'; exit 1; }
	@"$(CONTAINER)" volume inspect "$(BR_VOLUME)" >/dev/null 2>&1 || { \
		echo "creating the $(BR_VOLUME) volume ($(BR_VOLUME_SIZE))"; \
		"$(CONTAINER)" volume create -s $(BR_VOLUME_SIZE) "$(BR_VOLUME)"; \
		"$(CONTAINER)" run --rm --uid 0 --gid 0 -v $(BR_VOLUME):/br "$(BR_IMAGE)" \
			chown -R $(shell id -u):$(shell id -g) /br; }

# The defconfig in git is the reference.  Each build applies it again.
rootfs: br-volume
	@$(BR_RUN) sh -c 'set -e; \
		cd /work/$(BR_DIR); \
		$(BR_MAKE) $(BR_DEFCONFIG); \
		$(KERNEL_SYNC); \
		$(BR_MAKE)'
	@$(MAKE) --no-print-directory br-artifacts

# Copy the kernel and its manifest from the volume for flashing, next to the
# loader and OpenSBI binaries: also the squashfs root of a board without a
# disk, and the disk images of a board with a disk.
br-artifacts:
	@$(BR_RUN) sh -c 'set -e; \
		mkdir -p /work/$(BR_OUT) /work/$(BUILD_DIR); \
		test $$(wc -c < $(BR_O)/images/Image) -le $$(( $(LINUX_SIZE) - 12 )) || \
			{ echo "Image and its manifest are larger than the $(LINUX_SIZE) partition"; exit 1; }; \
		cp $(BR_O)/images/Image $(BR_O)/images/linux.size /work/$(BUILD_DIR)/; \
		if test -f $(BR_O)/images/rootfs.squashfs; then \
			test $$(wc -c < $(BR_O)/images/rootfs.squashfs) -le $$(( $(ROOT_SIZE) )) || \
				{ echo "rootfs.squashfs is larger than the $(ROOT_SIZE) partition"; exit 1; }; \
			cp $(BR_O)/images/rootfs.squashfs /work/$(BUILD_DIR)/; \
		fi; \
		for f in rootfs.ext2 sdcard.img; do \
			if test -f $(BR_O)/images/$$f; then cp $(BR_O)/images/$$f /work/$(BR_OUT)/; fi; \
		done'

rootfs-menuconfig: br-volume
	@$(BR_RUN) -i -t sh -c 'cd /work/$(BR_DIR) && $(BR_MAKE) $(BR_DEFCONFIG) && $(BR_MAKE) menuconfig'

sdcard: rootfs
	@test -f "$(BR_OUT)/sdcard.img" || { \
		echo 'Buildroot produced no sdcard.img; check post-image.sh'; exit 1; }
	@ls -l "$(BR_OUT)/sdcard.img"

# Stop if SD_DISK is not set.
SD_DISK_REQUIRED = @test -n "$(SD_DISK)" || { \
	echo 'set SD_DISK=/dev/diskN (see: diskutil list external)'; exit 1; }
# Stop if the user does not type $(1).
SD_CONFIRM = @read -r -p 'Type $(1) to continue: ' reply; \
	test "$$reply" = $(1) || { echo aborted; exit 1; }
SD_SHOW = @diskutil info "$(SD_DISK)" | grep -E 'Device Node|Media Name|Disk Size|Removable Media'

# Partition a large disk: a large FAT partition for the Mac and a small ext4
# root.  Use this and not sdwrite when the disk is larger than the image.
# fsck.fat on the board needs approximately 8 bytes for each cluster, so p1
# gets 64 KiB clusters (128 sectors).  FAT32 then needs 4 GiB or more.
sdpart:
	$(SD_DISK_REQUIRED)
	$(SD_SHOW)
	@echo "This ERASES $(SD_DISK) and everything on it."
	$(call SD_CONFIRM,ERASE)
	@diskutil unmountDisk "$(SD_DISK)"
	@diskutil partitionDisk "$(SD_DISK)" MBR \
		"MS-DOS FAT32" ESP32S31 $(SD_DATA_SIZE) "MS-DOS FAT32" ROOTFS R \
		"MS-DOS FAT32" SWAP 64M
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo newfs_msdos -F 32 -c 128 -v ESP32S31 "$(SD_RAW)s1"
# The kernel ignores the type byte of p2.  With 0x83, macOS does not try to
# mount the ext4 partition as FAT at each insertion.  S05swap.sh uses p3 only
# with 0x82.  The disk signature of genimage.cfg (0x45533331, little endian)
# lets the Function-CoreBoard-1 kernel find its root stick.
	@sudo sh -c 'dd if="$(SD_RAW)" of="$(BUILD_DIR)/mbr.bin" bs=512 count=1 && \
		printf "\\x31\\x33\\x53\\x45" | dd of="$(BUILD_DIR)/mbr.bin" bs=1 seek=440 conv=notrunc && \
		printf "\\x83" | dd of="$(BUILD_DIR)/mbr.bin" bs=1 seek=466 conv=notrunc && \
		printf "\\x82" | dd of="$(BUILD_DIR)/mbr.bin" bs=1 seek=482 conv=notrunc && \
		dd if="$(BUILD_DIR)/mbr.bin" of="$(SD_RAW)" bs=512 count=1' \
		|| echo 'warning: MBR unchanged; no disk swap, and the CoreBoard-1 does not boot'
	@rm -f "$(BUILD_DIR)/mbr.bin"
	@diskutil list "$(SD_DISK)"
	@echo 'now: make sdroot SD_DISK=$(SD_DISK)'

# Erases the disk.  On a large disk, the space after p3 is not used.
sdwrite:
	$(SD_DISK_REQUIRED)
	@test -f "$(BR_OUT)/sdcard.img" || { echo 'no $(BR_OUT)/sdcard.img; run make sdcard'; exit 1; }
	$(SD_SHOW)
	@echo "This ERASES $(SD_DISK) and everything on it."
	$(call SD_CONFIRM,ERASE)
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo dd if="$(BR_OUT)/sdcard.img" of="$(SD_RAW)" bs=4m
	@sync
	@diskutil eject "$(SD_DISK)"

# For updates: does not change the data partition or the partition sizes.
sdroot:
	$(SD_DISK_REQUIRED)
	@test -f "$(BR_OUT)/rootfs.ext2" || { echo 'no $(BR_OUT)/rootfs.ext2; run make rootfs'; exit 1; }
	@test -e "$(SD_DISK)s2" || { \
		echo '$(SD_DISK)s2 does not exist; run make sdpart first'; exit 1; }
	@part=$$(diskutil info -plist "$(SD_DISK)s2" | plutil -extract TotalSize raw -) && \
		test "$$part" -ge $$(stat -f %z "$(BR_OUT)/rootfs.ext2") || { \
		echo '$(SD_DISK)s2 is smaller than rootfs.ext2; run make sdpart or sdwrite'; exit 1; }
	@echo "This overwrites the root partition $(SD_DISK)s2."
	$(call SD_CONFIRM,WRITE)
	@diskutil unmountDisk "$(SD_DISK)"
	@sudo dd if="$(BR_OUT)/rootfs.ext2" of="$(SD_RAW)s2" bs=4m
	@sync
	@diskutil eject "$(SD_DISK)"

# Offset and file of each flash region.  The release package uses the same
# list (make flash-args).
FLASH_IMAGES = \
	$(BOOTLOADER_OFFSET) $(BUILD_DIR)/bootloader/bootloader/bootloader.bin \
	$(PARTITION_TABLE_OFFSET) $(BUILD_DIR)/bootloader/partition_table/partition-table.bin \
	$(APP_OFFSET) $(BUILD_DIR)/bootloader/s31-linux-loader.bin \
	$(OPENSBI_OFFSET) $(BUILD_DIR)/opensbi.bin \
	$(LINUX_OFFSET) $(BUILD_DIR)/Image \
	$(LINUX_SIZE_OFFSET) $(BUILD_DIR)/linux.size \
	$(if $(ROOT_OFFSET),$(ROOT_OFFSET) $(BUILD_DIR)/rootfs.squashfs)

flash: build
	@test -n "$(FLASH_PORT)" || { echo 'set FLASH_PORT=/dev/cu.<flash-port>'; exit 1; }
	$(ESPTOOL_CHECK)
	@$(ESPTOOL) --chip esp32s31 -p "$(FLASH_PORT)" -b 921600 \
		--before default-reset --after hard-reset write-flash $(FLASH_IMAGES)

flash-args:
	@printf '%s %s\n' $(FLASH_IMAGES)

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
