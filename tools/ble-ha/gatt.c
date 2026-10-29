// SPDX-License-Identifier: MIT
/*
 * An ATT server with a fixed GATT database: the GAP service and the
 * Improv Wi-Fi service.  Only the requests that clients send to find and
 * use a service are here.
 */

#include <string.h>

#include "ble-ha.h"

enum { SVC, DECL, VALUE, CCCD };

#define PROP_READ	0x02
#define PROP_WNR	0x04
#define PROP_WRITE	0x08
#define PROP_NOTIFY	0x10

#define ERR_INVALID_HANDLE	0x01
#define ERR_READ_NOT_PERM	0x02
#define ERR_WRITE_NOT_PERM	0x03
#define ERR_NOT_SUPPORTED	0x06
#define ERR_INVALID_OFFSET	0x07
#define ERR_QUEUE_FULL		0x09
#define ERR_NOT_FOUND		0x0a
#define ERR_INVALID_LEN		0x0d

#define LAST_HANDLE	19

/* 00467768-6228-2272-4663-2774782680XX, in the byte order of the air. */
#define IMPROV(x) { x, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, \
		    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 }
#define U16(x) { (x) & 0xff, (x) >> 8 }

/* For SVC and DECL: the UUID that it declares.  For VALUE: the UUID of the characteristic. */
struct attr {
	uint8_t kind;
	uint8_t props;
	uint8_t uuid_len;
	uint8_t uuid[16];
};

static const struct attr db[LAST_HANDLE + 1] = {
	[1] = { SVC, 0, 2, U16(0x1800) },
	[2] = { DECL, PROP_READ, 2, U16(0x2a00) },
	[3] = { VALUE, PROP_READ, 2, U16(0x2a00) },
	[4] = { DECL, PROP_READ, 2, U16(0x2a01) },
	[5] = { VALUE, PROP_READ, 2, U16(0x2a01) },
	[6] = { SVC, 0, 16, IMPROV(0x00) },
	[7] = { DECL, PROP_READ | PROP_NOTIFY, 16, IMPROV(0x01) },
	[8] = { VALUE, PROP_READ | PROP_NOTIFY, 16, IMPROV(0x01) },
	[9] = { CCCD, 0, 0, { 0 } },
	[10] = { DECL, PROP_READ | PROP_NOTIFY, 16, IMPROV(0x02) },
	[11] = { VALUE, PROP_READ | PROP_NOTIFY, 16, IMPROV(0x02) },
	[12] = { CCCD, 0, 0, { 0 } },
	[13] = { DECL, PROP_WRITE | PROP_WNR, 16, IMPROV(0x03) },
	[14] = { VALUE, PROP_WRITE | PROP_WNR, 16, IMPROV(0x03) },
	[15] = { DECL, PROP_READ | PROP_NOTIFY, 16, IMPROV(0x04) },
	[16] = { VALUE, PROP_READ | PROP_NOTIFY, 16, IMPROV(0x04) },
	[17] = { CCCD, 0, 0, { 0 } },
	[18] = { DECL, PROP_READ, 16, IMPROV(0x05) },
	[19] = { VALUE, PROP_READ, 16, IMPROV(0x05) },
};

void gatt_init(struct gatt *g, const char *name, uint8_t caps,
	       void (*rpc_write)(void *, const uint8_t *, size_t), void *ctx)
{
	memset(g, 0, sizeof(*g));
	g->name = name;
	g->caps = caps;
	g->state = IMPROV_AUTHORIZED;
	g->mtu = g->mtu_max = 23;
	g->rpc_write = rpc_write;
	g->ctx = ctx;
}

static int cccd_index(uint16_t h)
{
	return h == 9 ? 0 : h == 12 ? 1 : h == 17 ? 2 : -1;
}

int gatt_notify_enabled(const struct gatt *g, uint16_t value_handle)
{
	int i = cccd_index(value_handle + 1);

	return i >= 0 && (g->cccd[i] & 1);
}

/* The type of an attribute: 0x2800, 0x2803, 0x2902, or the characteristic UUID. */
static size_t attr_type(uint16_t h, uint8_t *uuid)
{
	static const uint8_t svc[2] = U16(0x2800), decl[2] = U16(0x2803), cccd[2] = U16(0x2902);
	const struct attr *a = &db[h];

	switch (a->kind) {
	case SVC:
		memcpy(uuid, svc, 2);
		return 2;
	case DECL:
		memcpy(uuid, decl, 2);
		return 2;
	case CCCD:
		memcpy(uuid, cccd, 2);
		return 2;
	default:
		memcpy(uuid, a->uuid, a->uuid_len);
		return a->uuid_len;
	}
}

/* The value of an attribute, or -ERR_* when it cannot be read. */
static int attr_value(const struct gatt *g, uint16_t h, uint8_t *v)
{
	const struct attr *a = &db[h];
	int i;

	if (h < 1 || h > LAST_HANDLE)
		return -ERR_INVALID_HANDLE;
	switch (a->kind) {
	case SVC:
		memcpy(v, a->uuid, a->uuid_len);
		return a->uuid_len;
	case DECL:
		v[0] = a->props;
		v[1] = (h + 1) & 0xff;
		v[2] = (uint8_t)((h + 1) >> 8);
		memcpy(v + 3, a->uuid, a->uuid_len);
		return 3 + a->uuid_len;
	case CCCD:
		i = cccd_index(h);
		v[0] = g->cccd[i] & 0xff;
		v[1] = (uint8_t)(g->cccd[i] >> 8);
		return 2;
	}
	switch (h) {
	case 3:
		i = (int)strlen(g->name);
		memcpy(v, g->name, (size_t)i);
		return i;
	case 5:
		v[0] = v[1] = 0;
		return 2;
	case GATT_STATE_VALUE:
		v[0] = g->state;
		return 1;
	case GATT_ERROR_VALUE:
		v[0] = g->error;
		return 1;
	case GATT_RESULT_VALUE:
		memcpy(v, g->result, g->result_len);
		return (int)g->result_len;
	case 19:
		v[0] = g->caps;
		return 1;
	}
	return -ERR_READ_NOT_PERM;
}

static size_t error_rsp(uint8_t *rsp, uint8_t op, uint16_t h, uint8_t err)
{
	rsp[0] = 0x01;
	rsp[1] = op;
	rsp[2] = h & 0xff;
	rsp[3] = (uint8_t)(h >> 8);
	rsp[4] = err;
	return 5;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t group_end(uint16_t h)
{
	uint16_t e = h + 1;

	while (e <= LAST_HANDLE && db[e].kind != SVC)
		e++;
	return e - 1;
}

/* Read By Group Type and Read By Type: entries of one length, as many as fit. */
static size_t read_by_type(const struct gatt *g, const uint8_t *req, size_t len,
			   uint8_t *rsp, size_t max)
{
	uint16_t start = get16(req + 1), end = get16(req + 3), h;
	uint8_t want[16], type[16], v[64];
	size_t wl = len - 5, pos = 2, entry = 0;
	int group = req[0] == 0x10;

	if ((wl != 2 && wl != 16) || !start || start > end)
		return error_rsp(rsp, req[0], start, wl != 2 && wl != 16 ? ERR_INVALID_LEN : ERR_INVALID_HANDLE);
	memcpy(want, req + 5, wl);
	for (h = start; h <= end && h <= LAST_HANDLE; h++) {
		int vl;
		size_t el;

		if (attr_type(h, type) != wl || memcmp(type, want, wl))
			continue;
		vl = attr_value(g, h, v);
		if (vl < 0) {
			if (entry)
				break;
			return error_rsp(rsp, req[0], h, (uint8_t)-vl);
		}
		el = (group ? 4 : 2) + (size_t)vl;
		if (entry && el != entry)
			break;
		if (pos + el > max)
			break;
		put16(rsp + pos, h);
		if (group) {
			put16(rsp + pos + 2, group_end(h));
			memcpy(rsp + pos + 4, v, (size_t)vl);
		} else {
			memcpy(rsp + pos + 2, v, (size_t)vl);
		}
		pos += el;
		entry = el;
	}
	if (!entry)
		return error_rsp(rsp, req[0], start, ERR_NOT_FOUND);
	rsp[0] = req[0] + 1;
	rsp[1] = (uint8_t)entry;
	return pos;
}

static size_t find_info(const uint8_t *req, uint8_t *rsp, size_t max)
{
	uint16_t start = get16(req + 1), end = get16(req + 3), h;
	uint8_t type[16];
	size_t pos = 2, tl = 0;

	if (!start || start > end)
		return error_rsp(rsp, req[0], start, ERR_INVALID_HANDLE);
	for (h = start; h <= end && h <= LAST_HANDLE; h++) {
		size_t l = attr_type(h, type);

		if ((tl && l != tl) || pos + 2 + l > max)
			break;
		put16(rsp + pos, h);
		memcpy(rsp + pos + 2, type, l);
		pos += 2 + l;
		tl = l;
	}
	if (!tl)
		return error_rsp(rsp, req[0], start, ERR_NOT_FOUND);
	rsp[0] = 0x05;
	rsp[1] = tl == 2 ? 1 : 2;
	return pos;
}

/* Find By Type Value: only primary services by UUID. */
static size_t find_by_value(const uint8_t *req, size_t len, uint8_t *rsp, size_t max)
{
	uint16_t start = get16(req + 1), end = get16(req + 3), h;
	size_t vl = len - 7, pos = 1;

	if (!start || start > end)
		return error_rsp(rsp, req[0], start, ERR_INVALID_HANDLE);
	for (h = start; h <= end && h <= LAST_HANDLE; h++) {
		if (get16(req + 5) != 0x2800 || db[h].kind != SVC ||
		    db[h].uuid_len != vl || memcmp(db[h].uuid, req + 7, vl) || pos + 4 > max)
			continue;
		put16(rsp + pos, h);
		put16(rsp + pos + 2, group_end(h));
		pos += 4;
	}
	if (pos == 1)
		return error_rsp(rsp, req[0], start, ERR_NOT_FOUND);
	rsp[0] = 0x07;
	return pos;
}

static size_t read_value(const struct gatt *g, const uint8_t *req, size_t len,
			 uint8_t *rsp, size_t max)
{
	uint16_t h = get16(req + 1), off = req[0] == 0x0c && len >= 5 ? get16(req + 3) : 0;
	uint8_t v[64];
	int vl = attr_value(g, h, v);
	size_t n;

	if (vl < 0)
		return error_rsp(rsp, req[0], h, (uint8_t)-vl);
	if (off > vl)
		return error_rsp(rsp, req[0], h, ERR_INVALID_OFFSET);
	n = (size_t)vl - off;
	if (n > max - 1)
		n = max - 1;
	rsp[0] = req[0] + 1;
	memcpy(rsp + 1, v + off, n);
	return n + 1;
}

/* Returns the ATT error, or 0. */
static uint8_t write_value(struct gatt *g, uint16_t h, const uint8_t *v, size_t len)
{
	int i = cccd_index(h);

	if (i >= 0) {
		if (len != 2)
			return ERR_INVALID_LEN;
		g->cccd[i] = get16(v);
		return 0;
	}
	if (h == GATT_RPC_VALUE) {
		g->rpc_write(g->ctx, v, len);
		return 0;
	}
	return h < 1 || h > LAST_HANDLE ? ERR_INVALID_HANDLE : ERR_WRITE_NOT_PERM;
}

/* Handle one ATT request; returns the length of the response, 0 for none. */
size_t gatt_handle(struct gatt *g, const uint8_t *req, size_t len,
		   uint8_t *rsp, size_t max)
{
	uint8_t err;

	if (!len)
		return 0;
	if (max > g->mtu)
		max = g->mtu;
	switch (req[0]) {
	case 0x02:	/* Exchange MTU */
		if (len < 3)
			break;
		g->mtu = get16(req + 1) < g->mtu_max ? get16(req + 1) : g->mtu_max;
		if (g->mtu < 23)
			g->mtu = 23;
		rsp[0] = 0x03;
		put16(rsp + 1, g->mtu_max);
		return 3;
	case 0x04:	/* Find Information */
		return len == 5 ? find_info(req, rsp, max) : error_rsp(rsp, req[0], 0, ERR_INVALID_LEN);
	case 0x06:	/* Find By Type Value */
		return len >= 7 ? find_by_value(req, len, rsp, max) : error_rsp(rsp, req[0], 0, ERR_INVALID_LEN);
	case 0x08:	/* Read By Type */
	case 0x10:	/* Read By Group Type */
		return len >= 7 ? read_by_type(g, req, len, rsp, max) : error_rsp(rsp, req[0], 0, ERR_INVALID_LEN);
	case 0x0a:	/* Read */
	case 0x0c:	/* Read Blob */
		return len >= 3 ? read_value(g, req, len, rsp, max) : error_rsp(rsp, req[0], 0, ERR_INVALID_LEN);
	case 0x12:	/* Write Request */
		if (len < 3)
			return error_rsp(rsp, req[0], 0, ERR_INVALID_LEN);
		err = write_value(g, get16(req + 1), req + 3, len - 3);
		if (err)
			return error_rsp(rsp, req[0], get16(req + 1), err);
		rsp[0] = 0x13;
		return 1;
	case 0x52:	/* Write Command */
		if (len >= 3)
			write_value(g, get16(req + 1), req + 3, len - 3);
		return 0;
	case 0x16:	/* Prepare Write: only the RPC command, in order */
		if (len < 5)
			return error_rsp(rsp, req[0], 0, ERR_INVALID_LEN);
		if (get16(req + 1) != GATT_RPC_VALUE)
			return error_rsp(rsp, req[0], get16(req + 1), ERR_WRITE_NOT_PERM);
		if (get16(req + 3) != g->prep_len)
			return error_rsp(rsp, req[0], get16(req + 1), ERR_INVALID_OFFSET);
		if (g->prep_len + len - 5 > sizeof(g->prep))
			return error_rsp(rsp, req[0], get16(req + 1), ERR_QUEUE_FULL);
		memcpy(g->prep + g->prep_len, req + 5, len - 5);
		g->prep_len += len - 5;
		memcpy(rsp, req, len < max ? len : max);
		rsp[0] = 0x17;
		return len < max ? len : max;
	case 0x18:	/* Execute Write */
		if (len >= 2 && req[1] == 1 && g->prep_len)
			g->rpc_write(g->ctx, g->prep, g->prep_len);
		memset(g->prep, 0, g->prep_len);
		g->prep_len = 0;
		rsp[0] = 0x19;
		return 1;
	case 0x1e:	/* Handle Value Confirmation */
		return 0;
	}
	/* Commands (bit 6) get no response. */
	return req[0] & 0x40 ? 0 : error_rsp(rsp, req[0], 0, ERR_NOT_SUPPORTED);
}
