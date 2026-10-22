/* SPDX-License-Identifier: (GPL-2.0-only OR Apache-2.0) */
/*
 * ESP-Hosted shared-memory transport for the ESP32-S31: Linux on hart 1 is
 * the host, the ESP-Hosted firmware on hart 0 is the device.  The
 * firmware, the Linux driver and OpenSBI include this header.
 *
 * The block is in internal SRAM, which the two harts access without the data
 * cache.  Each direction is a byte ring of frames.  A frame is the same as on
 * the USB transport: struct esp_payload_header, then the payload.  Each record
 * is a 32-bit frame length, then the frame, padded to 4 bytes.  head and tail
 * count bytes and wrap at 2^32.  The producer writes head, the consumer
 * writes tail.  The doorbells are the cross-core interrupts: Linux sets
 * FROM_CPU_1, and the firmware sets FROM_CPU_2.
 */
#ifndef _ESP32S31_HOSTED_SHM_H
#define _ESP32S31_HOSTED_SHM_H

#define ESP_SHM_MAGIC		0x4d485345	/* "ESHM" */
#define ESP_SHM_VERSION		2

#define ESP_SHM_ADDR		0x2f050000
#define ESP_SHM_SIZE		0x0000d000

/* Offsets in the block. */
#define ESP_SHM_CTRL		0x0000
#define ESP_SHM_TO_HOST_CTRL	0x0100
#define ESP_SHM_TO_FW_CTRL	0x0200
#define ESP_SHM_TO_FW_DATA	0x1000
#define ESP_SHM_TO_FW_SIZE	0x4000
#define ESP_SHM_TO_HOST_DATA	0x5000
#define ESP_SHM_TO_HOST_SIZE	0x8000

/* Control words, at ESP_SHM_CTRL.  The firmware writes magic last. */
#define ESP_SHM_MAGIC_OFF	0x00
#define ESP_SHM_VERSION_OFF	0x04
#define ESP_SHM_HOST_REQ_OFF	0x08	/* host requests, firmware clears */
/*
 * The cache sync engine holds one operation at a time.  OpenSBI and the
 * firmware hold this word at 1 for each operation.
 */
#define ESP_SHM_CACHE_LOCK_OFF	0x0c

/* Ring control words, at ESP_SHM_TO_*_CTRL. */
#define ESP_SHM_HEAD_OFF	0x00
#define ESP_SHM_TAIL_OFF	0x40
#define ESP_SHM_WAIT_OFF	0x80	/* producer waits for space */

/*
 * The flash mailbox, at ESP_SHM_FLASH: a window of the flash for Linux, as an
 * MTD device.  The window is the "rootfs" and "data" partitions of the
 * firmware.  At start, the firmware maps the window in the flash cache and
 * writes its size and address (size 0 when there is none).  Linux reads the
 * window there.  For a program or an erase, Linux writes ADDR (in the
 * window), LEN and for a program the data, then CMD, and rings.  The
 * firmware writes STATUS (0 or an esp_err_t), clears CMD and rings.  A
 * program is at most ESP_SHM_FLASH_DATA_SIZE bytes in one 4 KiB sector.  An
 * erase is one block of ESP_SHM_FLASH_BLOCK bytes.
 */
#define ESP_SHM_FLASH		0x0300
#define ESP_SHM_FLASH_SIZE_OFF	0x00
#define ESP_SHM_FLASH_CMD_OFF	0x04
#define ESP_SHM_FLASH_STATUS_OFF	0x08
#define ESP_SHM_FLASH_ADDR_OFF	0x0c
#define ESP_SHM_FLASH_LEN_OFF	0x10
#define ESP_SHM_FLASH_MAP_OFF	0x14
#define ESP_SHM_FLASH_DATA	0x0800
#define ESP_SHM_FLASH_DATA_SIZE	0x0800
#define ESP_SHM_FLASH_SECTOR	0x1000
#define ESP_SHM_FLASH_BLOCK	0x10000

#define ESP_SHM_FLASH_PROGRAM	2
#define ESP_SHM_FLASH_ERASE	3

/* Host request bits.  OpenSBI sets POWEROFF when Linux powers off. */
#define ESP_SHM_REQ_READY_REPLAY	(1U << 0)
#define ESP_SHM_REQ_POWEROFF		(1U << 1)

#define ESP_SHM_RECORD_LEN(frame_len)	(4 + (((frame_len) + 3) & ~3U))

#endif /* _ESP32S31_HOSTED_SHM_H */
