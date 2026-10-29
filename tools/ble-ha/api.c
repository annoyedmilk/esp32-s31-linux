// SPDX-License-Identifier: MIT
/*
 * The messages of the ESPHome native API (api.proto of ESPHome) that a
 * passive Bluetooth proxy needs, in plaintext frames: 0x00, the length and
 * the type as varints, then the protobuf message.
 */

#include <string.h>

#include "ble-ha.h"

/* The ESPHome release whose api.proto this follows. */
#define ESPHOME_VERSION	"2026.10.0"
#define API_MAJOR	1
#define API_MINOR	10
/* PASSIVE_SCAN | RAW_ADVERTISEMENTS */
#define PROXY_FEATURES	(1u << 0 | 1u << 5)

size_t pb_varint(uint8_t *p, uint64_t v)
{
	size_t n = 0;

	do {
		p[n] = (v & 0x7f) | (v > 0x7f ? 0x80 : 0);
		v >>= 7;
		n++;
	} while (v);
	return n;
}

static size_t pb_key(uint8_t *p, unsigned int field, unsigned int wire)
{
	return pb_varint(p, (uint64_t)field << 3 | wire);
}

/* proto3: a field with the default value is not sent. */
static size_t pb_uint(uint8_t *p, unsigned int field, uint64_t v)
{
	size_t n;

	if (!v)
		return 0;
	n = pb_key(p, field, 0);
	return n + pb_varint(p + n, v);
}

static size_t pb_bytes(uint8_t *p, unsigned int field, const void *data, size_t len)
{
	size_t n;

	if (!len)
		return 0;
	n = pb_key(p, field, 2);
	n += pb_varint(p + n, len);
	memcpy(p + n, data, len);
	return n + len;
}

static size_t pb_str(uint8_t *p, unsigned int field, const char *s)
{
	return s ? pb_bytes(p, field, s, strlen(s)) : 0;
}

size_t api_frame(uint8_t *buf, uint32_t type, const uint8_t *msg, size_t len)
{
	size_t n = 1;

	buf[0] = 0x00;
	n += pb_varint(buf + n, len);
	n += pb_varint(buf + n, type);
	memmove(buf + n, msg, len);
	return n + len;
}

/* HelloResponse */
size_t api_hello(uint8_t *buf, const struct api_info *info)
{
	uint8_t *p = buf;

	p += pb_uint(p, 1, API_MAJOR);
	p += pb_uint(p, 2, API_MINOR);
	p += pb_str(p, 3, "ble-ha (ESP32-S31 Linux)");
	p += pb_str(p, 4, info->name);
	return (size_t)(p - buf);
}

/* DeviceInfoResponse */
size_t api_device_info(uint8_t *buf, const struct api_info *info)
{
	uint8_t *p = buf;

	p += pb_str(p, 2, info->name);
	p += pb_str(p, 3, info->mac);
	p += pb_str(p, 4, ESPHOME_VERSION);
	p += pb_str(p, 6, info->model);
	/* With its own project, Home Assistant asks for no ESPHome update. */
	p += pb_str(p, 8, "esp32-s31-linux.ble-ha");
	p += pb_str(p, 9, "1");
	p += pb_str(p, 12, "Espressif");
	p += pb_str(p, 13, info->name);
	p += pb_uint(p, 15, PROXY_FEATURES);
	p += pb_str(p, 18, info->bt_mac);
	return (size_t)(p - buf);
}

/* BluetoothLERawAdvertisementsResponse */
size_t api_adv_batch(uint8_t *buf, const struct adv *ads, int n)
{
	uint8_t *p = buf, sub[64];
	int i;

	for (i = 0; i < n; i++) {
		const struct adv *a = &ads[i];
		/* sint32: zigzag */
		uint32_t rssi = ((uint32_t)a->rssi << 1) ^ (uint32_t)(a->rssi >> 31);
		uint8_t *q = sub;

		q += pb_uint(q, 1, a->addr);
		q += pb_uint(q, 2, rssi);
		q += pb_uint(q, 3, a->addr_type);
		q += pb_bytes(q, 4, a->data, a->len);
		p += pb_key(p, 1, 2);
		p += pb_varint(p, (uint64_t)(q - sub));
		memcpy(p, sub, (size_t)(q - sub));
		p += q - sub;
	}
	return (size_t)(p - buf);
}

static int read_varint(const uint8_t *p, size_t len, size_t *pos, uint32_t *v)
{
	int shift;

	*v = 0;
	for (shift = 0; shift < 35; shift += 7) {
		if (*pos >= len)
			return 0;
		*v |= (uint32_t)(p[*pos] & 0x7f) << shift;
		if (!(p[(*pos)++] & 0x80))
			return 1;
	}
	return -1;
}

/* Returns the length of the frame, 0 when it is not complete, -1 when it is bad. */
int api_next(const uint8_t *buf, size_t len, uint32_t *type,
	     const uint8_t **msg, size_t *msg_len)
{
	size_t pos = 1;
	uint32_t n;
	int r;

	if (!len)
		return 0;
	if (buf[0] != 0x00)
		return -1;
	r = read_varint(buf, len, &pos, &n);
	if (r <= 0)
		return r;
	r = read_varint(buf, len, &pos, type);
	if (r <= 0)
		return r;
	if (n > 4096)
		return -1;
	if (pos + n > len)
		return 0;
	*msg = buf + pos;
	*msg_len = n;
	return (int)(pos + n);
}
