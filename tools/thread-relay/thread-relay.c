// SPDX-License-Identifier: MIT
/*
 * thread-relay: give the Spinel stream of the IEEE 802.15.4 RCP to one TCP
 * client, for otbr-agent of the OpenThread Border Router app of Home
 * Assistant (network_device).  A new client replaces the old one.  When
 * the device goes away (the transport of the firmware starts again and
 * registers it again), the client must reset the RCP: close the client,
 * and open the device again.
 */

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
#include <termios.h>
#include <unistd.h>

#include "relay.h"

#define DEFAULT_PORT	6638

static void usage(const char *name)
{
	fprintf(stderr, "usage: %s [-p PORT] DEVICE\n", name);
}

static int open_device(const char *path)
{
	struct termios tio;
	int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);

	/* A tty (a pseudo-terminal in the tests) must pass the bytes as they are. */
	if (fd >= 0 && tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
		tcsetattr(fd, TCSANOW, &tio);
	}
	return fd;
}

/*
 * Non-blocking: a connection that is reset before accept() must not
 * stop the relay in accept().
 */
static int listen_on(int fd, struct sockaddr *addr, socklen_t len)
{
	int on = 1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && bind(fd, addr, len) == 0 &&
	    listen(fd, 1) == 0)
		return fd;
	close(fd);
	return -1;
}

/* IPv6 with IPv4-mapped addresses, else IPv4 only. */
static int open_listener(int port)
{
	struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
	struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_port = htons(port) };
	int off = 0;
	int fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd >= 0) {
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
		fd = listen_on(fd, (struct sockaddr *)&a6, sizeof(a6));
		if (fd >= 0)
			return fd;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	return fd < 0 ? -1 : listen_on(fd, (struct sockaddr *)&a4, sizeof(a4));
}

/* Spinel frames are small and the RCP waits for them: no Nagle delay. */
static void tune_client(int fd)
{
	int on = 1;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	relay_dead_peer_timeout(fd);
}

static int write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

static int accept_client(int lfd, int old)
{
	int fd = accept(lfd, NULL, NULL);

	if (fd < 0)
		return old;
	/* The client socket blocks: only the listener must not. */
	fcntl(fd, F_SETFL, 0);
	if (old >= 0) {
		syslog(LOG_INFO, "a new client replaces the old one");
		close(old);
	} else {
		syslog(LOG_INFO, "client connected");
	}
	tune_client(fd);
	return fd;
}

/* Returns when the device fails, with the client closed. */
static void relay(int dev, int lfd)
{
	struct pollfd pfd[3] = {
		{ .fd = dev, .events = POLLIN },
		{ .fd = lfd, .events = POLLIN },
		{ .fd = -1, .events = POLLIN },
	};
	char buf[2048];
	ssize_t n;

	for (;;) {
		if (poll(pfd, 3, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[1].revents & POLLIN) {
			int old = pfd[2].fd;

			pfd[2].fd = accept_client(lfd, old);
			/* The events were for the old client, not for the new one. */
			if (pfd[2].fd != old)
				pfd[2].revents = 0;
		}

		if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			n = read(dev, buf, sizeof(buf));
			if (n <= 0 && !(n < 0 && errno == EINTR)) {
				syslog(LOG_ERR, "device: %s", n ? strerror(errno) : "end of stream");
				break;
			}
			/* Without a client, the frames of the RCP have no reader. */
			if (n > 0 && pfd[2].fd >= 0 && write_all(pfd[2].fd, buf, (size_t)n)) {
				syslog(LOG_INFO, "client gone: %s", strerror(errno));
				close(pfd[2].fd);
				pfd[2].fd = -1;
			}
		}

		if (pfd[2].fd >= 0 && (pfd[2].revents & (POLLIN | POLLERR | POLLHUP))) {
			n = read(pfd[2].fd, buf, sizeof(buf));
			if (n <= 0 && !(n < 0 && errno == EINTR)) {
				syslog(LOG_INFO, "client gone: %s", n ? strerror(errno) : "closed");
				close(pfd[2].fd);
				pfd[2].fd = -1;
			} else if (n > 0 && write_all(dev, buf, (size_t)n)) {
				syslog(LOG_ERR, "device: %s", strerror(errno));
				break;
			}
		}
	}
	if (pfd[2].fd >= 0)
		close(pfd[2].fd);
}

int main(int argc, char **argv)
{
	int port = DEFAULT_PORT;
	int opt, dev, lfd, err = 0;

	while ((opt = getopt(argc, argv, "p:h")) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (optind != argc - 1 || port <= 0 || port > 65535) {
		usage(argv[0]);
		return 2;
	}

	openlog("thread-relay", LOG_PERROR, LOG_DAEMON);
	/* A client that goes away must not end the relay. */
	signal(SIGPIPE, SIG_IGN);

	lfd = open_listener(port);
	if (lfd < 0) {
		syslog(LOG_ERR, "port %d: %s", port, strerror(errno));
		return 1;
	}
	for (;;) {
		dev = open_device(argv[optind]);
		if (dev < 0) {
			/* One message for each time the device is away. */
			if (errno != err)
				syslog(LOG_ERR, "%s: %s", argv[optind], strerror(errno));
			err = errno;
			sleep(1);
			continue;
		}
		err = 0;
		syslog(LOG_INFO, "%s on TCP port %d", argv[optind], port);
		relay(dev, lfd);
		close(dev);
		sleep(1);
	}
}
