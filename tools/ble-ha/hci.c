// SPDX-License-Identifier: MIT
/* The HCI packets that ble-ha reads and writes. */

#include <string.h>

#include "ble-ha.h"

/* 00467768-6228-2272-4663-277478268000, in the byte order of the air. */
static const uint8_t improv_uuid[16] = {
	0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
	0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00,
};

/*
 * An LE Advertising Report event (after the packet type): event code,
 * length, subevent 0x02, the number of reports, then each report as the
 * kernel reads it: type, address type, address, data length, data, RSSI.
 */
int hci_parse_adv_reports(const uint8_t *ev, size_t len, struct adv *out, int max)
{
	size_t pos = 4;
	int i, n;

	if (len < 4 || ev[0] != 0x3e || ev[2] != 0x02)
		return 0;
	n = ev[3];
	for (i = 0; i < n && i < max; i++) {
		struct adv *a = &out[i];
		int b;

		if (pos + 9 > len || ev[pos + 8] > ADV_MAX || pos + 10 + ev[pos + 8] > len)
			return i;
		a->addr_type = ev[pos + 1];
		a->addr = 0;
		for (b = 5; b >= 0; b--)
			a->addr = a->addr << 8 | ev[pos + 2 + b];
		a->len = ev[pos + 8];
		memcpy(a->data, ev + pos + 9, a->len);
		a->rssi = (int8_t)ev[pos + 9 + a->len];
		pos += 10 + a->len;
	}
	return i;
}

/* Flags, the Improv service UUID and its service data: 31 bytes. */
size_t hci_improv_adv_data(uint8_t *buf, uint8_t state, uint8_t caps)
{
	uint8_t *p = buf;

	*p++ = 2, *p++ = 0x01, *p++ = 0x06;
	*p++ = 17, *p++ = 0x07;
	memcpy(p, improv_uuid, 16);
	p += 16;
	*p++ = 9, *p++ = 0x16, *p++ = 0x77, *p++ = 0x46;
	*p++ = state, *p++ = caps;
	memset(p, 0, 4);
	p += 4;
	return (size_t)(p - buf);
}

/* The complete local name, for the scan response. */
size_t hci_name_data(uint8_t *buf, const char *name)
{
	size_t n = strlen(name);

	if (n > ADV_MAX - 2)
		n = ADV_MAX - 2;
	buf[0] = (uint8_t)(n + 1);
	buf[1] = 0x09;
	memcpy(buf + 2, name, n);
	return n + 2;
}

/* One ACL packet with one L2CAP frame: a start fragment that the controller does not flush. */
size_t hci_acl_frame(uint8_t *buf, uint16_t handle, uint16_t cid,
		     const uint8_t *data, size_t len)
{
	size_t l2 = len + 4;

	buf[0] = HCI_ACL;
	buf[1] = handle & 0xff;
	buf[2] = (handle >> 8) & 0x0f;
	buf[3] = l2 & 0xff;
	buf[4] = (uint8_t)(l2 >> 8);
	buf[5] = len & 0xff;
	buf[6] = (uint8_t)(len >> 8);
	buf[7] = cid & 0xff;
	buf[8] = (uint8_t)(cid >> 8);
	memcpy(buf + 9, data, len);
	return len + 9;
}
