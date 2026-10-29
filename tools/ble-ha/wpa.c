// SPDX-License-Identifier: MIT
/*
 * The control socket of wpa_supplicant on wlan0: add a network from Improv,
 * wait for its connection, then save it or remove it.  The passphrase goes
 * over the socket, not on a command line that other users can read.
 */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ble-ha.h"

#define WPA_CTRL	"/var/run/wpa_supplicant/wlan0"

static int request(const char *cmd, char *reply, size_t len)
{
	struct sockaddr_un local = { .sun_family = AF_UNIX };
	struct sockaddr_un dest = { .sun_family = AF_UNIX };
	struct pollfd pfd;
	ssize_t n = -1;
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);

	if (fd < 0)
		return -1;
	snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/ble-ha-wpa-%d", (int)getpid());
	snprintf(dest.sun_path, sizeof(dest.sun_path), "%s", WPA_CTRL);
	unlink(local.sun_path);
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0 &&
	    connect(fd, (struct sockaddr *)&dest, sizeof(dest)) == 0 &&
	    send(fd, cmd, strlen(cmd), 0) >= 0) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 3000) == 1)
			n = recv(fd, reply, len - 1, 0);
	}
	close(fd);
	unlink(local.sun_path);
	if (n < 0)
		return -1;
	reply[n] = '\0';
	return 0;
}

static int request_ok(const char *cmd)
{
	char r[64];

	return request(cmd, r, sizeof(r)) == 0 && strncmp(r, "OK", 2) == 0 ? 0 : -1;
}

/* The number of networks in the config, or -1 without wpa_supplicant. */
int wpa_networks(void)
{
	char r[2048], *p;
	int n = -1;

	if (request("LIST_NETWORKS", r, sizeof(r)))
		return -1;
	/* A header line, then one line for each network. */
	for (p = r; (p = strchr(p, '\n')); p++)
		n++;
	return n < 0 ? 0 : n;
}

/* Add the network and connect to it alone.  Returns its id, or -1. */
int wpa_join(const char *ssid, const char *pass)
{
	char cmd[160], r[32];
	size_t i, pos;
	int id, err;

	if (request("ADD_NETWORK", r, sizeof(r)) || !(r[0] >= '0' && r[0] <= '9'))
		return -1;
	id = atoi(r);
	/* The SSID in hex: any byte, no quoting. */
	pos = (size_t)snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid ", id);
	for (i = 0; ssid[i] && pos + 3 < sizeof(cmd); i++)
		pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, "%02x", (unsigned char)ssid[i]);
	if (request_ok(cmd))
		goto fail;
	if (*pass) {
		/* The passphrase, not the PSK: SAE (WPA3) needs it. */
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", id, pass);
		err = request_ok(cmd);
		memset(cmd, 0, sizeof(cmd));
		if (err)
			goto fail;
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt WPA-PSK SAE", id);
	} else {
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
	}
	if (request_ok(cmd))
		goto fail;
	snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	if (request_ok(cmd))
		goto fail;
	return id;
fail:
	wpa_finish(id, 0);
	return -1;
}

int wpa_connected(char *ip, size_t len)
{
	char r[1024], *p;

	if (request("STATUS", r, sizeof(r)))
		return -1;
	if (!strstr(r, "wpa_state=COMPLETED\n") || !(p = strstr(r, "\nip_address=")))
		return 0;
	p += strlen("\nip_address=");
	snprintf(ip, len, "%.*s", (int)strcspn(p, "\n"), p);
	return 1;
}

/* Keep the network and save the config, or remove it.  Then all networks are on again. */
int wpa_finish(int id, int keep)
{
	char cmd[32];

	if (!keep) {
		snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
		request_ok(cmd);
	}
	request_ok("ENABLE_NETWORK all");
	return keep ? request_ok("SAVE_CONFIG") : 0;
}
