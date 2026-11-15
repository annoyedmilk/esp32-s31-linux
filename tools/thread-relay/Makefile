# SPDX-License-Identifier: MIT
# thread-relay for the target.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra

thread-relay: thread-relay.c
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ thread-relay.c

clean:
	rm -f thread-relay

.PHONY: clean
