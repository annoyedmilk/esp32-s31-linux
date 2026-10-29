// SPDX-License-Identifier: MIT
/*
 * The RPC packets of Improv Wi-Fi (https://www.improv-wifi.com/ble/):
 * command, data length, data, then the low byte of the sum of the bytes
 * before it.
 */

#include <string.h>

#include "ble-ha.h"

static uint8_t checksum(const uint8_t *p, size_t len)
{
	unsigned int sum = 0;

	while (len--)
		sum += *p++;
	return (uint8_t)sum;
}

/* A WPA passphrase: 8 to 63 printable ASCII characters.  Empty: an open network. */
static int valid_pass(const uint8_t *p, size_t len)
{
	size_t i;

	if (len && (len < 8 || len > 63))
		return 0;
	for (i = 0; i < len; i++)
		if (p[i] < 0x20 || p[i] > 0x7e)
			return 0;
	return 1;
}

static int parse_wifi(const uint8_t *d, size_t len, struct improv_cmd *c)
{
	size_t sl, pl;

	if (len < 2)
		return IMPROV_ERR_INVALID;
	sl = d[0];
	if (sl == 0 || sl > 32 || 1 + sl + 1 > len)
		return IMPROV_ERR_INVALID;
	pl = d[1 + sl];
	if (2 + sl + pl != len || !valid_pass(d + 2 + sl, pl) || memchr(d + 1, 0, sl))
		return IMPROV_ERR_INVALID;
	memcpy(c->ssid, d + 1, sl);
	c->ssid[sl] = '\0';
	memcpy(c->pass, d + 2 + sl, pl);
	c->pass[pl] = '\0';
	return 0;
}

int improv_parse(const uint8_t *buf, size_t len, struct improv_cmd *c)
{
	size_t dl;

	if (len < 3)
		return 1;
	dl = buf[1];
	if (len < dl + 3)
		return 1;
	if (len > dl + 3 || checksum(buf, dl + 2) != buf[dl + 2])
		return IMPROV_ERR_INVALID;
	c->cmd = buf[0];
	switch (c->cmd) {
	case IMPROV_CMD_WIFI:
		return parse_wifi(buf + 2, dl, c);
	case IMPROV_CMD_IDENTIFY:
	case IMPROV_CMD_INFO:
		return dl ? IMPROV_ERR_INVALID : 0;
	default:
		return IMPROV_ERR_UNKNOWN;
	}
}

size_t improv_result(uint8_t *buf, uint8_t cmd, const char *const *s, int n)
{
	size_t pos = 2;
	int i;

	buf[0] = cmd;
	for (i = 0; i < n; i++) {
		size_t l = strlen(s[i]);

		if (l > 40 || pos + 1 + l > 60)
			break;
		buf[pos++] = (uint8_t)l;
		memcpy(buf + pos, s[i], l);
		pos += l;
	}
	buf[1] = (uint8_t)(pos - 2);
	buf[pos] = checksum(buf, pos);
	return pos + 1;
}
