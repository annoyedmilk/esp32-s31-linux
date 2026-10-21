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
	for dev in /dev/${disk}1 /dev/$disk; do
		[ -b $dev ] || continue
		# flush writes the data soon, so a stick pulled without umount
		# loses less.
		mount -t vfat -o rw,noatime,flush $dev $MNT 2>/dev/null || continue
		logger -t usb "$dev on $MNT"
		return
	done
	logger -t usb "no FAT file system on /dev/$disk"
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
	if [ "$ACTION" != remove ]; then
		mount_usb "$MDEV"
	elif grep -q "^/dev/$MDEV $MNT " /proc/mounts; then
		# Open files keep the old mount until they close.
		umount -l $MNT
		logger -t usb "/dev/$MDEV removed"
	fi
	;;
esac

exit 0
