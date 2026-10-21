#!/bin/sh
# SPDX-License-Identifier: MIT
# p2 on the disk (the card or the USB stick) is the root.  The kernel mounts
# it.  p1 is the FAT area that the Mac also writes.  It is optional: never
# stop the boot for it.

. /etc/default/disk
[ -n "${DISK_FAT:-}" ] || exit 0
P1=$DISK_FAT

# After a reset, the dirty flag of p1 stays set, and the kernel warns at each
# mount.  fsck.fat checks p1 and clears the flag.  It needs approximately 8
# bytes of memory for each cluster, so check only when that fits.
check_p1()
{
	set -- $(od -An -tu1 -v -N66 $P1 2>/dev/null)
	[ $# -eq 66 ] || return
	cluster=$(( (${12} + ${13} * 256) * ${14} ))
	[ "$cluster" -gt 0 ] || return
	# The flags are at byte 37 on FAT12/16 and at byte 65 on FAT32, which
	# has no 16-bit FAT size.
	if [ $(( ${23} + ${24} * 256 )) -ne 0 ]; then flags=${38}; else flags=${66}; fi
	[ $(( flags & 1 )) -eq 1 ] || return

	clusters=$(( $(cat /sys/class/block/${P1##*/}/size) * 512 / cluster ))
	avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
	if [ $(( clusters * 8 / 1024 )) -lt $(( avail / 2 )) ]; then
		fsck.fat -a $P1 >/dev/null 2>&1
		echo "disk: $P1 was not unmounted cleanly, checked"
	else
		echo "disk: $P1 was not unmounted cleanly; check it on a computer"
	fi
}

case "$1" in
start)
	[ -b $P1 ] || exit 0
	# A mounted FAT is always dirty, and fsck.fat must not change it.
	while read -r dev _; do
		[ "$dev" = "$P1" ] && exit 0
	done < /proc/mounts
	check_p1
	mkdir -p $DISK_MNT
	if mount -t vfat -o rw,noatime $P1 $DISK_MNT 2>/dev/null; then
		echo "disk: $P1 on $DISK_MNT"
	else
		echo "disk: $P1 will not mount"
	fi
	;;
stop)
	mountpoint -q $DISK_MNT && umount $DISK_MNT
	;;
esac

exit 0
