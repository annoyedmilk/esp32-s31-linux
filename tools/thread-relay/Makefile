# SPDX-License-Identifier: MIT
# thread-relay for the target, and a host build for the tests: make test.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra
HOSTCC ?= cc

thread-relay: thread-relay.c sockopt-linux.c relay.h
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ thread-relay.c sockopt-linux.c

thread-relay-host: thread-relay.c sockopt-none.c relay.h
	$(HOSTCC) $(WFLAGS) -Werror -O2 -o $@ thread-relay.c sockopt-none.c

test: thread-relay-host
	python3 tests/test_relay.py ./thread-relay-host

clean:
	rm -f thread-relay thread-relay-host

.PHONY: test clean
