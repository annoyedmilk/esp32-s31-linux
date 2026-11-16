# SPDX-License-Identifier: MIT
# A static binary: one file for each stage, so libc does not move the number.

CFLAGS ?= -O2
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra

pipe-rtt: pipe-rtt.c
	$(CC) $(WFLAGS) $(CFLAGS) -static -o $@ pipe-rtt.c

clean:
	rm -f pipe-rtt

.PHONY: clean
