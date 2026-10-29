// SPDX-License-Identifier: MIT
/* The HCI user channel: ble-ha owns the adapter, without bluetoothd. */

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ble-ha.h"

/* From the Linux Bluetooth headers, which the C library does not have. */
#define BTPROTO_HCI		1
#define HCI_CHANNEL_USER	1
#define HCIDEVDOWN		_IOW('H', 202, int)

struct sockaddr_hci {
	sa_family_t hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

int hci_open_user(int dev)
{
	struct sockaddr_hci a = {
		.hci_family = AF_BLUETOOTH,
		.hci_dev = (unsigned short)dev,
		.hci_channel = HCI_CHANNEL_USER,
	};
	int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	int err;

	if (fd < 0)
		return -1;
	/* The user channel needs the adapter down. */
	ioctl(fd, HCIDEVDOWN, dev);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
		return fd;
	err = errno;
	close(fd);
	errno = err;
	return -1;
}
