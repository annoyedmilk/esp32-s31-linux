# SPDX-License-Identifier: MIT
# ble-ha for the target, and host tests of its packet code: make test.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra
HOSTCC ?= cc
SRCS = hci.c api.c gatt.c improv.c
HDRS = ble-ha.h

ble-ha: ble-ha.c wpa.c hci-linux.c $(SRCS) $(HDRS)
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ ble-ha.c wpa.c hci-linux.c $(SRCS)

ble-ha-test: tests/test.c $(SRCS) $(HDRS)
	$(HOSTCC) $(WFLAGS) -Werror -O2 -I. -o $@ tests/test.c $(SRCS)

test: ble-ha-test
	./ble-ha-test

clean:
	rm -f ble-ha ble-ha-test

.PHONY: test clean
