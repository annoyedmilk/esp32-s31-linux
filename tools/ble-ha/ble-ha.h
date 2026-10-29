/* SPDX-License-Identifier: MIT */
#ifndef BLE_HA_H
#define BLE_HA_H

#include <stddef.h>
#include <stdint.h>

/* HCI packet types on the user channel. */
#define HCI_CMD		0x01
#define HCI_ACL		0x02
#define HCI_EVT		0x04

#define ADV_MAX		31
#define ADV_BATCH	16

struct adv {
	uint64_t addr;
	uint8_t addr_type;
	int8_t rssi;
	uint8_t len;
	uint8_t data[ADV_MAX];
};

/* hci.c: packets of the controller, and the commands. */
int hci_parse_adv_reports(const uint8_t *ev, size_t len, struct adv *out, int max);
size_t hci_improv_adv_data(uint8_t *buf, uint8_t state, uint8_t caps);
size_t hci_name_data(uint8_t *buf, const char *name);
size_t hci_acl_frame(uint8_t *buf, uint16_t handle, uint16_t cid,
		     const uint8_t *data, size_t len);

/* hci-linux.c: the user channel of an adapter. */
int hci_open_user(int dev);

/* api.c: the ESPHome native API, plaintext. */
struct api_info {
	const char *name;
	const char *mac;
	const char *bt_mac;
	const char *model;
};

size_t pb_varint(uint8_t *p, uint64_t v);
size_t api_frame(uint8_t *buf, uint32_t type, const uint8_t *msg, size_t len);
size_t api_hello(uint8_t *buf, const struct api_info *info);
size_t api_device_info(uint8_t *buf, const struct api_info *info);
size_t api_adv_batch(uint8_t *buf, const struct adv *ads, int n);
/* One message of buf: its type and payload.  0 when incomplete, -1 when bad. */
int api_next(const uint8_t *buf, size_t len, uint32_t *type,
	     const uint8_t **msg, size_t *msg_len);

/* gatt.c: the ATT server with the Improv service. */
#define GATT_STATE_VALUE	8
#define GATT_ERROR_VALUE	11
#define GATT_RPC_VALUE		14
#define GATT_RESULT_VALUE	16

struct gatt {
	const char *name;
	uint8_t state, error, caps;
	uint8_t result[64];
	size_t result_len;
	uint16_t cccd[3];	/* state, error, result */
	uint16_t mtu, mtu_max;	/* in use; the largest that one ACL packet takes */
	uint8_t prep[128];	/* prepared writes of the RPC command */
	size_t prep_len;
	/* A write to the RPC command characteristic. */
	void (*rpc_write)(void *ctx, const uint8_t *data, size_t len);
	void *ctx;
};

void gatt_init(struct gatt *g, const char *name, uint8_t caps,
	       void (*rpc_write)(void *, const uint8_t *, size_t), void *ctx);
size_t gatt_handle(struct gatt *g, const uint8_t *req, size_t len,
		   uint8_t *rsp, size_t max);
int gatt_notify_enabled(const struct gatt *g, uint16_t value_handle);

/* improv.c: the RPC packets of Improv Wi-Fi. */
#define IMPROV_AUTHORIZED	0x02
#define IMPROV_PROVISIONING	0x03
#define IMPROV_PROVISIONED	0x04

#define IMPROV_ERR_NONE		0x00
#define IMPROV_ERR_INVALID	0x01
#define IMPROV_ERR_UNKNOWN	0x02
#define IMPROV_ERR_CONNECT	0x03
#define IMPROV_ERR_OTHER	0xff

#define IMPROV_CMD_WIFI		0x01
#define IMPROV_CMD_IDENTIFY	0x02
#define IMPROV_CMD_INFO		0x03

#define IMPROV_CAP_IDENTIFY	0x01
#define IMPROV_CAP_INFO		0x02

struct improv_cmd {
	uint8_t cmd;
	char ssid[33];
	char pass[64];
};

/* 0 complete, 1 incomplete, else an IMPROV_ERR_* value. */
int improv_parse(const uint8_t *buf, size_t len, struct improv_cmd *c);
size_t improv_result(uint8_t *buf, uint8_t cmd, const char *const *s, int n);

/* wpa.c: the control socket of wpa_supplicant. */
int wpa_networks(void);
int wpa_join(const char *ssid, const char *pass);
/* 1 connected with an address, 0 not yet, -1 error. */
int wpa_connected(char *ip, size_t len);
int wpa_finish(int id, int keep);

#endif
