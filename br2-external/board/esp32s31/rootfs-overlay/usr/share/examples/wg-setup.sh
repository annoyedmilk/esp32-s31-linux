#!/bin/sh
# SPDX-License-Identifier: MIT
# Make a WireGuard key pair on the board and a template of
# /etc/wireguard/wg0.conf (mode 600), and print the public key for the
# peer.  Then put the peer into wg0.conf, set WG=yes and the address in
# /etc/default/wireguard, and run /etc/init.d/S42wireguard restart.
# With a port, the board is also a server on that UDP port.
#
#   wg-setup.sh [LISTEN_PORT]

CONF=/etc/wireguard/wg0.conf

[ $# -le 1 ] || { echo "usage: $0 [LISTEN_PORT]" >&2; exit 1; }
[ -e $CONF ] && { echo "$0: $CONF exists" >&2; exit 1; }
umask 077
mkdir -p /etc/wireguard
key=$(wg genkey) || exit 1
{
	echo "[Interface]"
	echo "PrivateKey = $key"
	[ -n "$1" ] && echo "ListenPort = $1"
	echo
	echo "[Peer]"
	echo "PublicKey = PEER_PUBLIC_KEY"
	echo "Endpoint = PEER_HOST:51820"
	echo "AllowedIPs = 10.0.0.1/32"
	echo "PersistentKeepalive = 25"
} > $CONF
echo "public key of this board: $(echo "$key" | wg pubkey)"
echo "now edit $CONF and /etc/default/wireguard"
