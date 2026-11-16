# SPDX-License-Identifier: MIT
# nft_allow SET "ADDRESS...": fill the sets SET4 and SET6 of inet filter with
# the IPv4 and the IPv6 addresses.  An empty list empties both.
nft_allow()
{
	nft flush set inet filter "${1}4" &&
		nft flush set inet filter "${1}6" || return 1
	for a in $2; do
		case $a in
		*:*) nft add element inet filter "${1}6" "{ $a }" ;;
		*) nft add element inet filter "${1}4" "{ $a }" ;;
		esac || return 1
	done
}
