// SPDX-License-Identifier: MIT
/* Host tests of the packet code of ble-ha. */

#include <stdio.h>
#include <string.h>

#include "ble-ha.h"

static int failed, count;

#define CHECK(cond) do { count++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_varint(void)
{
	uint8_t b[10];

	CHECK(pb_varint(b, 0) == 1 && b[0] == 0);
	CHECK(pb_varint(b, 300) == 2 && b[0] == 0xac && b[1] == 0x02);
	CHECK(pb_varint(b, 0xffffffffffffull) == 7);
}

static void test_frames(void)
{
	static const uint8_t ping[] = { 0x00, 0x00, 0x07 };
	uint8_t buf[64], m[2] = { 0x08, 0x01 };
	const uint8_t *msg;
	size_t mlen, n;
	uint32_t type;

	CHECK(api_next(ping, 3, &type, &msg, &mlen) == 3 && type == 7 && mlen == 0);
	CHECK(api_next(ping, 2, &type, &msg, &mlen) == 0);
	n = api_frame(buf, 66, m, 2);
	CHECK(n == 5 && buf[0] == 0 && buf[1] == 2 && buf[2] == 66);
	CHECK(api_next(buf, n, &type, &msg, &mlen) == 5 && type == 66 && mlen == 2 && msg[1] == 1);
	CHECK(api_next(buf, n - 1, &type, &msg, &mlen) == 0);
	buf[0] = 0x01;	/* the preamble of an encrypted frame */
	CHECK(api_next(buf, n, &type, &msg, &mlen) == -1);
}

static void test_adv_batch(void)
{
	struct adv a = { .addr = 0x112233445566ull, .addr_type = 1, .rssi = -60, .len = 3,
			 .data = { 0x02, 0x01, 0x06 } };
	static const uint8_t want[] = {
		0x0a, 0x11,				/* advertisements, 17 bytes */
		0x08, 0xe6, 0xaa, 0x91, 0x9a, 0xa3, 0xa4, 0x04,	/* address */
		0x10, 0x77,				/* rssi -60, zigzag 119 */
		0x18, 0x01,				/* address_type */
		0x22, 0x03, 0x02, 0x01, 0x06,		/* data */
	};
	uint8_t buf[64];

	CHECK(api_adv_batch(buf, &a, 1) == sizeof(want) && !memcmp(buf, want, sizeof(want)));
}

static void test_device_info(void)
{
	struct api_info info = { "esp32s31", "AA:BB:CC:DD:EE:FF", "11:22:33:44:55:66", "Board" };
	uint8_t buf[512];
	size_t n = api_device_info(buf, &info);

	/* name, then mac_address; feature flags 33 (field 15) */
	CHECK(buf[0] == 0x12 && buf[1] == 8 && !memcmp(buf + 2, "esp32s31", 8));
	CHECK(buf[10] == 0x1a && buf[11] == 17);
	CHECK(memmem(buf, n, "\x78\x21", 2) != NULL);
	CHECK(api_hello(buf, &info) > 0 && buf[0] == 0x08 && buf[1] == 1 && buf[2] == 0x10 && buf[3] == 10);
}

static void test_adv_reports(void)
{
	static const uint8_t ev[] = {
		0x3e, 0x11, 0x02, 0x01,			/* LE meta, advertising report, 1 */
		0x00, 0x01, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,	/* ADV_IND, random */
		0x03, 0x02, 0x01, 0x06, 0xc4,		/* 3 bytes of data, rssi -60 */
	};
	struct adv a[2];

	CHECK(hci_parse_adv_reports(ev, sizeof(ev), a, 2) == 1);
	CHECK(a[0].addr == 0x112233445566ull && a[0].addr_type == 1);
	CHECK(a[0].rssi == -60 && a[0].len == 3 && a[0].data[2] == 0x06);
	CHECK(hci_parse_adv_reports(ev, sizeof(ev) - 1, a, 2) == 0);
}

static void test_improv_adv(void)
{
	uint8_t d[31];

	CHECK(hci_improv_adv_data(d, IMPROV_AUTHORIZED, 3) == 31);
	CHECK(d[3] == 17 && d[4] == 0x07 && d[5] == 0x00 && d[20] == 0x00 && d[19] == 0x46);
	CHECK(d[21] == 9 && d[22] == 0x16 && d[23] == 0x77 && d[24] == 0x46);
	CHECK(d[25] == IMPROV_AUTHORIZED && d[26] == 3);
}

static uint8_t sum(const uint8_t *p, size_t n)
{
	unsigned int s = 0;

	while (n--)
		s += *p++;
	return (uint8_t)s;
}

static void test_improv_rpc(void)
{
	uint8_t pkt[64] = { 0x01, 0, 4, 'h', 'o', 'm', 'e', 8, 's', 'e', 'c', 'r', 'e', 't', '1', '2' };
	struct improv_cmd c;
	const char *str[2] = { "a", "bc" };
	uint8_t res[64];
	size_t n;

	pkt[1] = 14;
	pkt[16] = sum(pkt, 16);
	CHECK(improv_parse(pkt, 17, &c) == 0 && c.cmd == 1);
	CHECK(!strcmp(c.ssid, "home") && !strcmp(c.pass, "secret12"));
	CHECK(improv_parse(pkt, 10, &c) == 1);
	pkt[16]++;
	CHECK(improv_parse(pkt, 17, &c) == IMPROV_ERR_INVALID);
	/* A passphrase of 7 characters is not valid for WPA. */
	pkt[1] = 13, pkt[7] = 7, pkt[15] = sum(pkt, 15);
	CHECK(improv_parse(pkt, 16, &c) == IMPROV_ERR_INVALID);
	/* An open network: no passphrase. */
	pkt[1] = 6, pkt[7] = 0, pkt[8] = sum(pkt, 8);
	CHECK(improv_parse(pkt, 9, &c) == 0 && c.pass[0] == '\0');
	pkt[0] = 0x09, pkt[1] = 0, pkt[2] = 0x09;
	CHECK(improv_parse(pkt, 3, &c) == IMPROV_ERR_UNKNOWN);
	n = improv_result(res, 0x03, str, 2);
	CHECK(n == 8 && res[0] == 3 && res[1] == 5 && res[2] == 1 && res[4] == 2);
	CHECK(res[7] == sum(res, 7));
}

static uint8_t rpc_seen[128];
static size_t rpc_seen_len;

static void rpc(void *ctx, const uint8_t *d, size_t len)
{
	(void)ctx;
	memcpy(rpc_seen + rpc_seen_len, d, len);
	rpc_seen_len += len;
}

static void test_gatt(void)
{
	static const uint8_t mtu[] = { 0x02, 0xf7, 0x00 };
	static const uint8_t groups[] = { 0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28 };
	static const uint8_t groups2[] = { 0x10, 0x06, 0x00, 0xff, 0xff, 0x00, 0x28 };
	static const uint8_t chars[] = { 0x08, 0x06, 0x00, 0x13, 0x00, 0x03, 0x28 };
	static const uint8_t info[] = { 0x04, 0x09, 0x00, 0x09, 0x00 };
	static const uint8_t read_state[] = { 0x0a, 0x08, 0x00 };
	static const uint8_t cccd_on[] = { 0x12, 0x09, 0x00, 0x01, 0x00 };
	static const uint8_t write_rpc[] = { 0x12, 0x0e, 0x00, 0xaa, 0xbb };
	static const uint8_t prep1[] = { 0x16, 0x0e, 0x00, 0x00, 0x00, 0x01 };
	static const uint8_t prep2[] = { 0x16, 0x0e, 0x00, 0x01, 0x00, 0x02 };
	static const uint8_t exec[] = { 0x18, 0x01 };
	static const uint8_t read_name[] = { 0x0a, 0x03, 0x00 };
	struct gatt g;
	uint8_t r[256];
	size_t n;

	gatt_init(&g, "esp32s31", IMPROV_CAP_INFO, rpc, NULL);
	g.mtu_max = 247;
	n = gatt_handle(&g, mtu, sizeof(mtu), r, sizeof(r));
	CHECK(n == 3 && r[0] == 0x03 && r[1] == 0xf7 && g.mtu == 247);
	/* GAP (16-bit UUID), then Improv (128-bit) in a second response */
	n = gatt_handle(&g, groups, sizeof(groups), r, sizeof(r));
	CHECK(n == 8 && r[0] == 0x11 && r[1] == 6 && r[2] == 1 && r[4] == 5);
	n = gatt_handle(&g, groups2, sizeof(groups2), r, sizeof(r));
	CHECK(n == 22 && r[1] == 20 && r[2] == 6 && r[4] == 19 && r[21] == 0x00 && r[20] == 0x46);
	/* the five characteristics of Improv: 21-byte entries */
	n = gatt_handle(&g, chars, sizeof(chars), r, sizeof(r));
	CHECK(n == 2 + 5 * 21 && r[1] == 21 && r[2] == 7 && r[4] == 0x12 && r[5] == 8 && r[7] == 0x01);
	n = gatt_handle(&g, info, sizeof(info), r, sizeof(r));
	CHECK(n == 6 && r[0] == 0x05 && r[1] == 1 && r[2] == 9 && r[4] == 0x02 && r[5] == 0x29);
	n = gatt_handle(&g, read_state, sizeof(read_state), r, sizeof(r));
	CHECK(n == 2 && r[0] == 0x0b && r[1] == IMPROV_AUTHORIZED);
	n = gatt_handle(&g, read_name, sizeof(read_name), r, sizeof(r));
	CHECK(n == 9 && !memcmp(r + 1, "esp32s31", 8));
	CHECK(!gatt_notify_enabled(&g, GATT_STATE_VALUE));
	n = gatt_handle(&g, cccd_on, sizeof(cccd_on), r, sizeof(r));
	CHECK(n == 1 && r[0] == 0x13 && gatt_notify_enabled(&g, GATT_STATE_VALUE));
	n = gatt_handle(&g, write_rpc, sizeof(write_rpc), r, sizeof(r));
	CHECK(n == 1 && rpc_seen_len == 2 && rpc_seen[1] == 0xbb);
	rpc_seen_len = 0;
	gatt_handle(&g, prep1, sizeof(prep1), r, sizeof(r));
	gatt_handle(&g, prep2, sizeof(prep2), r, sizeof(r));
	n = gatt_handle(&g, exec, sizeof(exec), r, sizeof(r));
	CHECK(n == 1 && r[0] == 0x19 && rpc_seen_len == 2 && rpc_seen[0] == 1 && rpc_seen[1] == 2);
	/* the RPC command cannot be read */
	r[0] = 0x0a, r[1] = 0x0e, r[2] = 0;
	n = gatt_handle(&g, r, 3, r + 8, sizeof(r) - 8);
	CHECK(n == 5 && r[8] == 0x01 && r[12] == 0x02);
}

int main(void)
{
	test_varint();
	test_frames();
	test_adv_batch();
	test_device_info();
	test_adv_reports();
	test_improv_adv();
	test_improv_rpc();
	test_gatt();
	printf("%d checks, %d failed\n", count, failed);
	return failed != 0;
}
