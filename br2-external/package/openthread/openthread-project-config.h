/* SPDX-License-Identifier: MIT */
/*
 * OpenThread gives the border agent a random UDP port.  A fixed port lets
 * nftables.conf accept the commissioners.  thread-share gives the
 * ephemeral key (ePSKc) port 49192.
 */
#define OPENTHREAD_CONFIG_BORDER_AGENT_UDP_PORT 49191
