// SPDX-License-Identifier: MIT
/* A host build for the tests: the system default keepalive times. */

#include "relay.h"

void relay_dead_peer_timeout(int fd)
{
	(void)fd;
}
