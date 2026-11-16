# SPDX-License-Identifier: MIT
# ble-ha for the target.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra
SRCS = hci.c api.c gatt.c improv.c
HDRS = ble-ha.h

ble-ha: ble-ha.c wpa.c hci-linux.c $(SRCS) $(HDRS)
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ ble-ha.c wpa.c hci-linux.c $(SRCS)

clean:
	rm -f ble-ha

.PHONY: clean
