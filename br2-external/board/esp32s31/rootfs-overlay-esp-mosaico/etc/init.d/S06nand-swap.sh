#!/bin/sh
# SPDX-License-Identifier: MIT
# zram has priority 100. Use NAND after zram, with MTD wear levelling,
# bad-block handling and a synthetic header. Keep its OOB marker pages
# separate from swap data: on-die ECC does not permit reprogramming them.
# Never mkswap mtdblock.

[ "$1" = start ] || exit 0
[ -f /overlay/.storage-ready ] || exit 0
. /usr/libexec/nand-storage.sh
mtd=$(nand_mtd nand-swap 33554432) || exit 0
index=${mtd#mtd}
modprobe mtdswap partitions="$index" header=1 reserve_marker_pages=1 || exit 1
swapon -p 10 "/dev/mtdswap$index" || exit 1
echo "swap: /dev/mtdswap$index (NAND, priority 10) active"
