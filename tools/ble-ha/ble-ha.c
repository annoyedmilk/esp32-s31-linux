// SPDX-License-Identifier: MIT
/*
 * ble-ha: Bluetooth LE for Home Assistant.
 *
 *   ble-ha [-p] [-i] [-d DEV]
 *
 * -p: a passive Bluetooth proxy: the ESPHome native API on TCP 6053 gives
 *     the LE advertisements in range to Home Assistant.  The adapter scans
 *     only while a client subscribes.
 * -i: Improv Wi-Fi over BLE while wpa_supplicant has no network: a phone or
 *     Home Assistant gives the board its Wi-Fi network.
 *
 * ble-ha owns the adapter through the HCI user channel, so bluetoothd
 * cannot use it at the same time.  It ends when it has nothing to do.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ble-ha.h"

#define API_PORT	6053
#define MAX_CLIENTS	4
#define LED		"/sys/class/leds/rgb:status"
#define JOIN_SECONDS	30

struct client {
	int fd;
	uint8_t buf[1024];
	size_t len;
	int subscribed;
};

static struct {
	int hci;
	uint16_t acl_len, acl_credits;
	uint16_t conn_sent;	/* packets of the connection that the controller still has */
	uint8_t bdaddr[6];
	int scanning;
	struct adv batch[ADV_BATCH];
	int nbatch;
	long batch_ms;		/* when the first of the batch came */
	/* Improv */
	int improv, advertising, adv_dirty;
	int conn;
	uint8_t l2[256];
	size_t l2_len;
	uint8_t rpc[128];
	size_t rpc_len;
	struct gatt gatt;
	int join_id;
	time_t join_end, identify_end, done_at, check_at;
	/* The proxy */
	int proxy, lfd;
	struct client clients[MAX_CLIENTS];
	struct api_info info;
	char name[64], mac[18], bt_mac[18], model[64];
} s = { .hci = -1, .conn = -1, .lfd = -1, .join_id = -1 };

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static time_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- The adapter ---- */

static void handle_packet(const uint8_t *p, size_t len);

static ssize_t hci_read(uint8_t *buf, size_t len, int ms)
{
	struct pollfd pfd = { .fd = s.hci, .events = POLLIN };

	if (poll(&pfd, 1, ms) != 1)
		return 0;
	return read(s.hci, buf, len);
}

/*
 * Send a command and wait for its Command Complete or Command Status.
 * Other packets only change the state here: no command waits in another.
 * Returns the status, or -1.
 */
static int hci_cmd(uint16_t op, const void *param, uint8_t plen, uint8_t *ret, size_t rlen)
{
	uint8_t pkt[260];
	long end = now_ms() + 2000;

	pkt[0] = HCI_CMD;
	pkt[1] = op & 0xff;
	pkt[2] = (uint8_t)(op >> 8);
	pkt[3] = plen;
	memcpy(pkt + 4, param, plen);
	if (write(s.hci, pkt, 4u + plen) != 4 + plen)
		return -1;
	while (now_ms() < end) {
		ssize_t n = hci_read(pkt, sizeof(pkt), (int)(end - now_ms()));

		if (n <= 0)
			continue;
		if (pkt[0] == HCI_EVT && pkt[1] == 0x0e && n >= 7 && (pkt[4] | pkt[5] << 8) == op) {
			if (ret)
				memcpy(ret, pkt + 6, (size_t)n - 6 < rlen ? (size_t)n - 6 : rlen);
			return pkt[6];
		}
		if (pkt[0] == HCI_EVT && pkt[1] == 0x0f && n >= 7 && (pkt[5] | pkt[6] << 8) == op)
			return pkt[3];
		handle_packet(pkt, (size_t)n);
	}
	syslog(LOG_ERR, "HCI command 0x%04x: no answer", op);
	return -1;
}

static int hci_init(void)
{
	/* Bit 61: LE meta events, with the default events below it. */
	static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
	static const uint8_t le_mask[8] = { 0x1f };
	uint8_t r[16];

	if (hci_cmd(0x0c03, NULL, 0, NULL, 0) ||		/* Reset */
	    hci_cmd(0x0c01, mask, 8, NULL, 0) ||		/* Set Event Mask */
	    hci_cmd(0x2001, le_mask, 8, NULL, 0) ||		/* LE Set Event Mask */
	    hci_cmd(0x1009, NULL, 0, r, sizeof(r)))		/* Read BD_ADDR */
		return -1;
	memcpy(s.bdaddr, r + 1, 6);
	if (hci_cmd(0x2002, NULL, 0, r, sizeof(r)) == 0 && (r[1] | r[2] << 8)) {
		s.acl_len = (uint16_t)(r[1] | r[2] << 8);	/* LE Read Buffer Size */
		s.acl_credits = r[3];
	} else if (hci_cmd(0x1005, NULL, 0, r, sizeof(r)) == 0) {
		s.acl_len = (uint16_t)(r[1] | r[2] << 8);	/* Read Buffer Size */
		s.acl_credits = (uint16_t)(r[4] | r[5] << 8);
	}
	if (s.acl_len < 27)
		s.acl_len = 27;
	return 0;
}

static void set_scan(int on)
{
	/* Active, every 320 ms for 30 ms: the radio also has Wi-Fi and 802.15.4. */
	static const uint8_t param[7] = { 0x01, 0x00, 0x02, 0x30, 0x00, 0x00, 0x00 };
	uint8_t en[2] = { (uint8_t)on, 0x00 };

	if (on == s.scanning)
		return;
	if (on && hci_cmd(0x200b, param, sizeof(param), NULL, 0))
		return;
	if (hci_cmd(0x200c, en, sizeof(en), NULL, 0) == 0)
		s.scanning = on;
}

static void set_advertising(int on)
{
	/* ADV_IND every 100 to 200 ms on the three channels. */
	static const uint8_t param[15] = { 0xa0, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
					   0, 0, 0, 0, 0, 0, 0x07, 0x00 };
	uint8_t data[32] = { 0 }, en = (uint8_t)on;

	if (on) {
		data[0] = (uint8_t)hci_improv_adv_data(data + 1, s.gatt.state, s.gatt.caps);
		hci_cmd(0x2008, data, sizeof(data), NULL, 0);
		if (!s.advertising) {
			memset(data, 0, sizeof(data));
			data[0] = (uint8_t)hci_name_data(data + 1, s.name);
			hci_cmd(0x2009, data, sizeof(data), NULL, 0);
			hci_cmd(0x2006, param, sizeof(param), NULL, 0);
		}
	}
	if (on != s.advertising && hci_cmd(0x200a, &en, 1, NULL, 0) == 0)
		s.advertising = on;
	s.adv_dirty = 0;
}

static void acl_send(uint16_t cid, const uint8_t *data, size_t len)
{
	uint8_t pkt[300];

	if (s.conn < 0 || len + 4 > s.acl_len || len + 9 > sizeof(pkt))
		return;
	if (!s.acl_credits) {
		syslog(LOG_WARNING, "no ACL buffer in the controller: packet dropped");
		return;
	}
	if (write(s.hci, pkt, hci_acl_frame(pkt, (uint16_t)s.conn, cid, data, len)) > 0) {
		s.acl_credits--;
		s.conn_sent++;
	}
}

/* ---- Improv ---- */

static void notify(uint16_t handle, const uint8_t *v, size_t len)
{
	uint8_t pdu[80];

	if (!gatt_notify_enabled(&s.gatt, handle))
		return;
	/* ATT sends the first MTU - 3 bytes; the client reads the rest. */
	if (len + 3 > s.gatt.mtu)
		len = s.gatt.mtu - 3u;
	if (len + 3 > sizeof(pdu))
		len = sizeof(pdu) - 3;
	pdu[0] = 0x1b;
	pdu[1] = handle & 0xff;
	pdu[2] = (uint8_t)(handle >> 8);
	memcpy(pdu + 3, v, len);
	acl_send(4, pdu, len + 3);
}

static void set_state(uint8_t state, uint8_t error)
{
	if (error != s.gatt.error) {
		s.gatt.error = error;
		notify(GATT_ERROR_VALUE, &error, 1);
	}
	if (state != s.gatt.state) {
		s.gatt.state = state;
		notify(GATT_STATE_VALUE, &state, 1);
		s.adv_dirty = 1;
	}
}

static void set_result(uint8_t cmd, const char *const *str, int n)
{
	s.gatt.result_len = improv_result(s.gatt.result, cmd, str, n);
	notify(GATT_RESULT_VALUE, s.gatt.result, s.gatt.result_len);
}

static void write_file(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, v, strlen(v)) < 0)
			syslog(LOG_WARNING, "%s: %s", path, strerror(errno));
		close(fd);
	}
}

static void identify(int on)
{
	write_file(LED "/trigger", on ? "timer" : "none");
	if (!on)
		write_file(LED "/brightness", "0");
	s.identify_end = on ? now() + 5 : 0;
}

static void improv_command(const struct improv_cmd *c)
{
	const char *info[4] = { "ble-ha", "1", "ESP32-S31", s.name };

	switch (c->cmd) {
	case IMPROV_CMD_WIFI:
		if (s.gatt.state != IMPROV_AUTHORIZED)
			return;
		set_state(IMPROV_PROVISIONING, IMPROV_ERR_NONE);
		s.join_id = wpa_join(c->ssid, c->pass);
		if (s.join_id < 0) {
			syslog(LOG_ERR, "improv: wpa_supplicant did not take the network");
			set_state(IMPROV_AUTHORIZED, IMPROV_ERR_OTHER);
			return;
		}
		syslog(LOG_INFO, "improv: joining a network");
		s.join_end = now() + JOIN_SECONDS;
		break;
	case IMPROV_CMD_IDENTIFY:
		if (s.gatt.caps & IMPROV_CAP_IDENTIFY)
			identify(1);
		break;
	case IMPROV_CMD_INFO:
		set_result(IMPROV_CMD_INFO, info, 4);
		break;
	}
}

/* A write to the RPC command: the packet can come in several writes. */
static void rpc_write(void *ctx, const uint8_t *data, size_t len)
{
	struct improv_cmd c;
	int r;

	(void)ctx;
	if (s.rpc_len + len > sizeof(s.rpc)) {
		s.rpc_len = 0;
		set_state(s.gatt.state, IMPROV_ERR_INVALID);
		return;
	}
	memcpy(s.rpc + s.rpc_len, data, len);
	s.rpc_len += len;
	r = improv_parse(s.rpc, s.rpc_len, &c);
	if (r == 1)
		return;
	memset(s.rpc, 0, s.rpc_len);
	s.rpc_len = 0;
	if (r) {
		set_state(s.gatt.state, (uint8_t)r);
		return;
	}
	set_state(s.gatt.state, IMPROV_ERR_NONE);
	improv_command(&c);
	memset(&c, 0, sizeof(c));
}

static void join_poll(void)
{
	char ip[48];
	int r = wpa_connected(ip, sizeof(ip));

	if (r == 1) {
		wpa_finish(s.join_id, 1);
		s.join_id = -1;
		syslog(LOG_INFO, "improv: connected, %s", ip);
		set_result(IMPROV_CMD_WIFI, NULL, 0);
		set_state(IMPROV_PROVISIONED, IMPROV_ERR_NONE);
		s.done_at = now() + 10;
	} else if (now() >= s.join_end) {
		wpa_finish(s.join_id, 0);
		s.join_id = -1;
		syslog(LOG_WARNING, "improv: no connection to the network");
		set_state(IMPROV_AUTHORIZED, IMPROV_ERR_CONNECT);
	}
}

/* ---- Packets of the adapter ---- */

static void l2cap_frame(uint16_t cid, const uint8_t *p, size_t len)
{
	uint8_t rsp[256];
	size_t n, max = (size_t)s.acl_len - 4;

	if (max > sizeof(rsp))
		max = sizeof(rsp);
	if (cid == 4) {
		n = gatt_handle(&s.gatt, p, len, rsp, max);
		if (n)
			acl_send(4, rsp, n);
	} else if (cid == 5 && len >= 4 && !(p[0] & 1)) {
		/* LE signaling: reject the requests. */
		uint8_t rej[6] = { 0x01, p[1], 0x02, 0x00, 0x00, 0x00 };

		acl_send(5, rej, sizeof(rej));
	} else if (cid == 6 && len >= 1 && (p[0] == 0x01 || p[0] == 0x0b)) {
		/* SMP: no pairing; Improv does not need it. */
		uint8_t fail[2] = { 0x05, 0x05 };

		acl_send(6, fail, sizeof(fail));
	}
}

static void handle_acl(const uint8_t *p, size_t len)
{
	size_t dl, need;

	if (len < 5)
		return;
	dl = (size_t)(p[3] | p[4] << 8);
	if (dl + 5 > len)
		return;
	if (((p[2] >> 4) & 3) != 1)	/* a start fragment */
		s.l2_len = 0;
	if (s.l2_len + dl > sizeof(s.l2)) {
		s.l2_len = 0;
		return;
	}
	memcpy(s.l2 + s.l2_len, p + 5, dl);
	s.l2_len += dl;
	if (s.l2_len < 4)
		return;
	need = (size_t)(s.l2[0] | s.l2[1] << 8) + 4;
	if (s.l2_len < need)
		return;
	l2cap_frame((uint16_t)(s.l2[2] | s.l2[3] << 8), s.l2 + 4, need - 4);
	s.l2_len = 0;
}

static void le_meta(const uint8_t *ev, size_t len)
{
	struct adv ads[8];
	int i, n;

	if (len < 3)
		return;
	if (ev[2] == 0x02 && s.scanning) {
		n = hci_parse_adv_reports(ev, len, ads, 8);
		if (n && !s.nbatch)
			s.batch_ms = now_ms();
		for (i = 0; i < n && s.nbatch < ADV_BATCH; i++)
			s.batch[s.nbatch++] = ads[i];
	} else if (ev[2] == 0x01 && len >= 6 && ev[3] == 0) {
		/* LE Connection Complete: the advertising has stopped. */
		s.conn = (ev[4] | ev[5] << 8) & 0x0fff;
		s.advertising = 0;
		s.l2_len = s.rpc_len = 0;
		s.gatt.mtu = 23;
		memset(s.gatt.cccd, 0, sizeof(s.gatt.cccd));
		s.gatt.prep_len = 0;
		syslog(LOG_INFO, "improv: a client connected");
	}
}

static void handle_packet(const uint8_t *p, size_t len)
{
	if (len < 3)
		return;
	if (p[0] == HCI_ACL) {
		handle_acl(p, len);
		return;
	}
	if (p[0] != HCI_EVT)
		return;
	switch (p[1]) {
	case 0x3e:
		le_meta(p + 1, len - 1);
		break;
	case 0x05:	/* Disconnection Complete */
		if (len >= 6 && ((p[4] | p[5] << 8) & 0x0fff) == s.conn) {
			/* The controller does not report the packets of a closed link. */
			s.acl_credits += s.conn_sent;
			s.conn_sent = 0;
			s.conn = -1;
			s.adv_dirty = 1;
		}
		break;
	case 0x13:	/* Number of Completed Packets */
		if (len >= 8) {
			uint16_t n = (uint16_t)(p[6] | p[7] << 8);

			s.acl_credits += n;
			s.conn_sent -= n < s.conn_sent ? n : s.conn_sent;
		}
		break;
	}
}

/* ---- The ESPHome API ---- */

static int open_listener(void)
{
	struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(API_PORT) };
	int off = 0, on = 1;
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (fcntl(fd, F_SETFL, O_NONBLOCK) || bind(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    listen(fd, MAX_CLIENTS)) {
		close(fd);
		return -1;
	}
	return fd;
}

static void client_close(struct client *c)
{
	close(c->fd);
	c->fd = -1;
	c->len = 0;
	c->subscribed = 0;
}

static void client_send(struct client *c, uint32_t type, const uint8_t *msg, size_t len)
{
	uint8_t buf[1200];
	size_t n;

	if (len + 12 > sizeof(buf))
		return;
	n = api_frame(buf, type, msg, len);
	/* A client that cannot take a message now goes: it connects again. */
	if (send(c->fd, buf, n, MSG_NOSIGNAL) != (ssize_t)n) {
		syslog(LOG_INFO, "API client too slow or gone");
		client_close(c);
	}
}

static void client_message(struct client *c, uint32_t type)
{
	uint8_t msg[512];

	switch (type) {
	case 1:		/* HelloRequest */
		client_send(c, 2, msg, api_hello(msg, &s.info));
		break;
	case 5:		/* DisconnectRequest */
		client_send(c, 6, NULL, 0);
		if (c->fd >= 0)
			client_close(c);
		break;
	case 7:		/* PingRequest */
		client_send(c, 8, NULL, 0);
		break;
	case 9:		/* DeviceInfoRequest */
		client_send(c, 10, msg, api_device_info(msg, &s.info));
		break;
	case 11:	/* ListEntitiesRequest: no entities */
		client_send(c, 19, NULL, 0);
		break;
	case 66:	/* SubscribeBluetoothLEAdvertisementsRequest */
		c->subscribed = 1;
		break;
	case 87:	/* UnsubscribeBluetoothLEAdvertisementsRequest */
		c->subscribed = 0;
		break;
	case 80:	/* SubscribeBluetoothConnectionsFreeRequest: no connections */
		client_send(c, 81, NULL, 0);
		break;
	}
}

static void client_read(struct client *c)
{
	const uint8_t *msg;
	size_t mlen, used = 0;
	uint32_t type;
	ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
	int r = 0;

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		client_close(c);
		return;
	}
	c->len += (size_t)n;
	while (c->fd >= 0 && (r = api_next(c->buf + used, c->len - used, &type, &msg, &mlen)) > 0) {
		client_message(c, type);
		used += (size_t)r;
	}
	if (c->fd < 0)
		return;
	if (r < 0 || (used == 0 && c->len == sizeof(c->buf))) {
		client_close(c);
		return;
	}
	memmove(c->buf, c->buf + used, c->len - used);
	c->len -= used;
}

/* A client that is gone without a close (power, Wi-Fi) goes within a minute. */
static void tune_client(int fd)
{
	int on = 1, idle = 20, intvl = 5, cnt = 4;
	unsigned int user_ms = 30000;

	fcntl(fd, F_SETFL, O_NONBLOCK);
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
	setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_ms, sizeof(user_ms));
}

static void accept_client(void)
{
	int fd = accept(s.lfd, NULL, NULL), i;

	if (fd < 0)
		return;
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (s.clients[i].fd < 0) {
			s.clients[i].fd = fd;
			tune_client(fd);
			return;
		}
	}
	close(fd);
}

static void flush_batch(void)
{
	uint8_t msg[ADV_BATCH * 48];
	size_t n;
	int i;

	if (!s.nbatch)
		return;
	n = api_adv_batch(msg, s.batch, s.nbatch);
	s.nbatch = 0;
	for (i = 0; i < MAX_CLIENTS; i++)
		if (s.clients[i].fd >= 0 && s.clients[i].subscribed)
			client_send(&s.clients[i], 93, msg, n);
}

static int subscribers(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_CLIENTS; i++)
		n += s.clients[i].fd >= 0 && s.clients[i].subscribed;
	return n;
}

/* ---- Set up and the loop ---- */

static void read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");

	buf[0] = '\0';
	if (f) {
		if (fgets(buf, (int)len, f))
			buf[strcspn(buf, "\n")] = '\0';
		fclose(f);
	}
}

static void setup_info(void)
{
	int i;

	gethostname(s.name, sizeof(s.name) - 1);
	read_line("/sys/class/net/wlan0/address", s.mac, sizeof(s.mac));
	for (i = 0; s.mac[i]; i++)
		if (s.mac[i] >= 'a' && s.mac[i] <= 'f')
			s.mac[i] -= 'a' - 'A';
	snprintf(s.bt_mac, sizeof(s.bt_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
		 s.bdaddr[5], s.bdaddr[4], s.bdaddr[3], s.bdaddr[2], s.bdaddr[1], s.bdaddr[0]);
	read_line("/proc/device-tree/model", s.model, sizeof(s.model));
	s.info.name = s.name;
	s.info.mac = s.mac;
	s.info.bt_mac = s.bt_mac;
	s.info.model = s.model;
}

static int running(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	char path[64], comm[32];
	int found = 0;

	while (d && !found && (e = readdir(d))) {
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%.20s/comm", e->d_name);
		read_line(path, comm, sizeof(comm));
		found = !strcmp(comm, name);
	}
	if (d)
		closedir(d);
	return found;
}

/* Improv only while wpa_supplicant has no network.  S40wifi starts it in the background. */
static int improv_needed(void)
{
	int i, n = -1;

	for (i = 0; i < 30 && (n = wpa_networks()) < 0 && !stop; i++)
		sleep(1);
	return n == 0;
}

/* A batch goes out 100 ms after its first advertisement, or when it is full. */
static int poll_timeout(void)
{
	long left = s.batch_ms + 100 - now_ms();

	if (s.nbatch)
		return left > 0 ? (int)left : 0;
	if (s.join_id >= 0 || s.identify_end || s.done_at)
		return 1000;
	return s.improv ? 5000 : -1;
}

static void timers(void)
{
	if (s.nbatch && now_ms() >= s.batch_ms + 100)
		flush_batch();
	if (s.join_id >= 0)
		join_poll();
	if (s.identify_end && now() >= s.identify_end)
		identify(0);
	/* After provisioning: no more advertising. */
	if (s.done_at && now() >= s.done_at && s.conn < 0) {
		s.improv = 0;
		s.done_at = 0;
		set_advertising(0);
	}
	/* Wi-Fi got a network in another way: no Improv for anyone in range. */
	if (s.improv && s.join_id < 0 && !s.done_at && s.conn < 0 && now() >= s.check_at) {
		s.check_at = now() + 5;
		if (wpa_networks() > 0) {
			syslog(LOG_INFO, "improv: Wi-Fi has a network; Improv ends");
			s.improv = 0;
			set_advertising(0);
		}
	}
}

static void run(void)
{
	struct pollfd pfd[2 + MAX_CLIENTS];
	uint8_t pkt[300];
	int i;

	while (!stop && (s.proxy || s.improv)) {
		if (s.adv_dirty && s.improv && s.conn < 0)
			set_advertising(1);
		set_scan(subscribers() > 0);
		pfd[0] = (struct pollfd){ .fd = s.hci, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = s.lfd, .events = POLLIN };
		for (i = 0; i < MAX_CLIENTS; i++)
			pfd[2 + i] = (struct pollfd){ .fd = s.clients[i].fd, .events = POLLIN };
		if (poll(pfd, 2 + MAX_CLIENTS, poll_timeout()) < 0 && errno != EINTR)
			break;
		if (pfd[0].revents & POLLIN) {
			ssize_t n = read(s.hci, pkt, sizeof(pkt));

			if (n < 0 && errno != EINTR) {
				syslog(LOG_ERR, "adapter: %s", strerror(errno));
				break;
			}
			if (n > 0)
				handle_packet(pkt, (size_t)n);
		}
		if (pfd[1].revents & POLLIN)
			accept_client();
		for (i = 0; i < MAX_CLIENTS; i++)
			if (s.clients[i].fd >= 0 && (pfd[2 + i].revents & (POLLIN | POLLHUP | POLLERR)))
				client_read(&s.clients[i]);
		if (s.nbatch >= ADV_BATCH)
			flush_batch();
		timers();
	}
}

int main(int argc, char **argv)
{
	int opt, dev = 0, want_improv = 0, i;

	while ((opt = getopt(argc, argv, "pid:h")) != -1) {
		switch (opt) {
		case 'p':
			s.proxy = 1;
			break;
		case 'i':
			want_improv = 1;
			break;
		case 'd':
			dev = atoi(optarg);
			break;
		default:
			fprintf(stderr, "usage: %s [-p] [-i] [-d DEV]\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	openlog("ble-ha", LOG_PERROR, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	for (i = 0; i < MAX_CLIENTS; i++)
		s.clients[i].fd = -1;

	s.improv = want_improv && improv_needed();
	if (!s.proxy && !s.improv) {
		syslog(LOG_INFO, "nothing to do: Wi-Fi has a network, and no proxy");
		return 0;
	}
	/* The user channel would take the adapter from bluetoothd and its devices. */
	if (running("bluetoothd")) {
		syslog(LOG_ERR, "bluetoothd has the adapter: stop it first");
		return 1;
	}
	s.hci = hci_open_user(dev);
	if (s.hci < 0 || hci_init()) {
		syslog(LOG_ERR, "hci%d: %s", dev, s.hci < 0 ? strerror(errno) : "no answer");
		return 1;
	}
	setup_info();
	gatt_init(&s.gatt, s.name, access(LED, F_OK) ? IMPROV_CAP_INFO
			: IMPROV_CAP_INFO | IMPROV_CAP_IDENTIFY, rpc_write, NULL);
	s.gatt.mtu_max = s.acl_len - 4 < 247 ? s.acl_len - 4 : 247;
	if (s.proxy) {
		s.lfd = open_listener();
		if (s.lfd < 0) {
			syslog(LOG_ERR, "port %d: %s", API_PORT, strerror(errno));
			return 1;
		}
	}
	if (s.improv)
		s.adv_dirty = 1;
	syslog(LOG_INFO, "hci%d %s:%s%s", dev, s.bt_mac, s.proxy ? " proxy on TCP 6053" : "",
	       s.improv ? " Improv Wi-Fi" : "");
	run();
	set_scan(0);
	set_advertising(0);
	if (s.join_id >= 0)
		wpa_finish(s.join_id, 0);
	syslog(LOG_INFO, "stopped");
	return 0;
}
