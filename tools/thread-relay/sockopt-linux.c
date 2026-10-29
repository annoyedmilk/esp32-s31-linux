// SPDX-License-Identifier: MIT
/* The keepalive times and the user timeout of Linux. */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "relay.h"

void relay_dead_peer_timeout(int fd)
{
	int idle = 5, intvl = 2, cnt = 3;
	unsigned int user_ms = 10000;

	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
	/* Also when data waits for an ACK, which keepalive does not cover. */
	setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_ms, sizeof(user_ms));
}
