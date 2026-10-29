// SPDX-License-Identifier: MIT
/* A host build for the tests has no Bluetooth adapter. */

#include <errno.h>

#include "ble-ha.h"

int hci_open_user(int dev)
{
	(void)dev;
	errno = ENOSYS;
	return -1;
}
