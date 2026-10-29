/* SPDX-License-Identifier: MIT */
#ifndef RELAY_H
#define RELAY_H

/* Close a connection whose peer is gone (power, Wi-Fi) within seconds. */
void relay_dead_peer_timeout(int fd);

#endif
