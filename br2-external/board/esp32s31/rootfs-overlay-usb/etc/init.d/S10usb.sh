#!/bin/sh
# SPDX-License-Identifier: MIT
# A USB stick is optional space for data, never the root.
# Mount the first FAT partition of the stick, or the full stick when it has
# no partition table, on /mnt/usb.  At start, mount a stick that is there,
# and make mdev the uevent helper: for a stick that comes later, mdev runs
# this script (mdev.conf).  When the stick goes, release the mount.

MNT=/mnt/usb

mount_usb()
{
	disk=${1%%[0-9]*}
	mountpoint -q $MNT && return
	mkdir -p $MNT
	dev=/dev/$disk
	[ -b ${dev}1 ] && dev=${dev}1
	# flush writes the data soon, so a stick pulled without umount loses
	# less.
	if mount -t vfat -o rw,noatime,flush $dev $MNT 2>/dev/null; then
		logger -t usb "$dev on $MNT"
	else
		logger -t usb "no FAT file system on $dev"
	fi
}

case "$1" in
start)
	echo /sbin/mdev > /proc/sys/kernel/hotplug
	[ -b /dev/sda ] && mount_usb sda
	;;
stop)
	mountpoint -q $MNT && umount $MNT
	;;
hotplug)
	# mdev runs the events of a stick in parallel, so only one of them
	# mounts: the first partition, or the full stick when it has none.  The
	# kernel adds the partitions before the event of the stick.
	if [ "$ACTION" != remove ]; then
		case $MDEV in
		sd?1) mount_usb "$MDEV" ;;
		*) [ -e /sys/block/$MDEV/${MDEV}1 ] || mount_usb "$MDEV" ;;
		esac
	elif grep -q "^/dev/$MDEV $MNT " /proc/mounts; then
		# Open files keep the old mount until they close.
		umount -l $MNT
		logger -t usb "/dev/$MDEV removed"
	fi
	;;
esac

exit 0
