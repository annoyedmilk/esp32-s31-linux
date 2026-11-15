# SPDX-License-Identifier: MIT
# ble-ha for the target.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra
SRCS = ble-ha.c wpa.c hci-linux.c hci.c api.c gatt.c improv.c

ble-ha: $(SRCS) ble-ha.h
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f ble-ha

.PHONY: clean
