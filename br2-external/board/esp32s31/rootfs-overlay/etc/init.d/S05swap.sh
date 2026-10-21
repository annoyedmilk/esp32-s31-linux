#!/bin/sh
# SPDX-License-Identifier: MIT
# Swap to LZ4-compressed RAM first, then to p3 of the disk when the MBR gives
# it the swap type.  zram needs its size in sysfs, so fstab cannot start it.
# The module alone has no disk and swaps to zram only.

ZRAM=/sys/block/zram0
ZRAM_SIZE=6M	# uncompressed
. /etc/default/disk

start_zram()
{
	echo lz4 > $ZRAM/comp_algorithm &&
		echo $ZRAM_SIZE > $ZRAM/disksize &&
		mkswap /dev/zram0 >/dev/null &&
		swapon -p 100 /dev/zram0 &&
		echo "swap: /dev/zram0 ($ZRAM_SIZE, lz4) active"
}

# Only type 0x82 (byte 482 of the MBR) marks p3 as swap: never format other
# data.  mkswap at each boot fits the header to the partition size.
start_disk()
{
	[ -b "${DISK_SWAP:-}" ] || return
	set -- $(dd if=$DISK bs=1 skip=482 count=1 2>/dev/null | od -An -tx1)
	[ "$1" = 82 ] || return
	mkswap $DISK_SWAP >/dev/null &&
		swapon -p 10 $DISK_SWAP &&
		echo "swap: $DISK_SWAP active"
}

case "$1" in
start)
	start_zram
	start_disk
	;;
esac

exit 0
