/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Shared-memory ABI between Linux on hart 1 and the ESP-IDF Wi-Fi firmware
 * on hart 0.  The two sides are on the same little-endian SoC and access this
 * region without the data cache.  Thus no byte swap and no cache maintenance
 * is necessary.  Each ring has one producer and one consumer: the producer
 * writes head, the consumer writes tail.  The slots have a fixed size, so no
 * memory ownership goes across the privilege boundary.
 *
 * This is the only copy of the file.  The build puts it into the kernel tree
 * next to the driver, and the loader includes it directly.
 */
#ifndef _ESP32S31_WIFI_IPC_H
#define _ESP32S31_WIFI_IPC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef int16_t s16;
#endif

#define ESP32S31_IPC_MAGIC		0x49313353	/* "S31I" */
#define ESP32S31_IPC_VERSION		3

/*
 * Fixed address of the block in internal SRAM.  The loader puts it here and
 * reserves it.  The wifi@ node in esp32s31.dtsi has the same address, and the
 * Linux driver gets it from there.
 */
#define ESP32S31_IPC_SRAM_ADDR		0x2f050000
#define ESP32S31_IPC_SRAM_SIZE		0x00010000

#define ESP32S31_IPC_LINE		64
#define ESP32S31_IPC_SLOT_DATA		1536
#define ESP32S31_IPC_SLOTS		16

struct esp32s31_ipc_slot {
	u32 len;
	u8 reserved[ESP32S31_IPC_LINE - 4];
	u8 data[ESP32S31_IPC_SLOT_DATA];
};

struct esp32s31_ipc_ring {
	u32 head;
	u8 reserved_head[ESP32S31_IPC_LINE - 4];
	u32 tail;
	u8 reserved_tail[ESP32S31_IPC_LINE - 4];
	struct esp32s31_ipc_slot slot[ESP32S31_IPC_SLOTS];
};

#define ESP32S31_IPC_SSID_MAX		32
#define ESP32S31_IPC_PSK_MAX		64

#define ESP32S31_IPC_CMD_NONE		0
#define ESP32S31_IPC_CMD_CONNECT	1
#define ESP32S31_IPC_CMD_DISCONNECT	2
#define ESP32S31_IPC_CMD_SCAN		3

#define ESP32S31_IPC_SCAN_MAX		24

/* Authentication modes: only the data that cfg80211 must publish. */
#define ESP32S31_IPC_AUTH_OPEN		0
#define ESP32S31_IPC_AUTH_SECURED	1

/*
 * The firmware runs the supplicant and does the association.  Linux writes
 * the credentials first and the command code last.
 */
struct esp32s31_ipc_cmd {
	u32 code;
	u8 ssid[ESP32S31_IPC_SSID_MAX];
	u8 psk[ESP32S31_IPC_PSK_MAX];
};

/*
 * One scan result.  The firmware gives the data that the host cannot find
 * itself.  cfg80211 makes the rest of the BSS entry from it.
 */
struct esp32s31_ipc_bss {
	u8 bssid[6];
	u8 channel;
	u8 ssid_len;
	u8 authmode;
	u8 reserved[1];
	s16 rssi;
	u8 ssid[ESP32S31_IPC_SSID_MAX];
};

/*
 * The firmware fills the table and then writes seq.  Linux compares seq with
 * the value at the time of its request, so it never uses an old table.
 */
struct esp32s31_ipc_scan {
	u32 seq;
	u32 count;
	struct esp32s31_ipc_bss bss[ESP32S31_IPC_SCAN_MAX];
};

struct esp32s31_ipc {
	u32 magic;
	u32 version;
	u32 link_up;
	u8 mac[6];
	/*
	 * The association that the firmware made.  cfg80211 does not accept
	 * a connection without a BSS.  Thus the host must know the AP and its
	 * channel to publish a BSS.
	 */
	u8 bssid[6];
	u8 channel;
	u8 reserved[ESP32S31_IPC_LINE - 25];
	struct esp32s31_ipc_cmd cmd;
	struct esp32s31_ipc_scan scan;
	struct esp32s31_ipc_ring to_linux;
	struct esp32s31_ipc_ring to_firmware;
};

#endif /* _ESP32S31_WIFI_IPC_H */
