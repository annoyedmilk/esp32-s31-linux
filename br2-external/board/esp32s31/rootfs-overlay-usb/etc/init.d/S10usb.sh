#!/bin/sh
# SPDX-License-Identifier: MIT
# A USB stick is optional space for data, never the root.
# Mount the first FAT or exFAT partition of the stick, or the full stick
# when it has no partition table, on /mnt/usb.  At start, make mdev the
# uevent helper, load the modules of the USB devices that are there (their
# boot events had no helper), and mount a stick that is there: for a stick
# that comes later, mdev runs this script (mdev.conf).  When the stick
# goes, release the mount.

MNT=/mnt/usb

mount_usb()
{
	disk=${1%%[0-9]*}
	mountpoint -q $MNT && return
	mkdir -p $MNT
	dev=/dev/$disk
	[ -b ${dev}1 ] && dev=${dev}1
	# flush writes the data soon: a stick pulled without umount loses less.
	# exFAT has no flush option.
	if mount -t vfat -o rw,noatime,flush $dev $MNT 2>/dev/null ||
	   mount -t exfat -o rw,noatime $dev $MNT 2>/dev/null; then
		logger -t usb "$dev on $MNT"
	else
		logger -t usb "no FAT or exFAT file system on $dev"
	fi
}

case "$1" in
start)
	echo /sbin/mdev > /proc/sys/kernel/hotplug
	cat /sys/bus/usb/devices/*/modalias 2>/dev/null | sort -u | xargs -r modprobe -q -a
	[ -b /dev/sda ] && mount_usb sda
	;;
stop)
	mountpoint -q $MNT && umount $MNT
	;;
hotplug)
	# The events run in parallel: mount only p1, or the stick without a p1.
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
