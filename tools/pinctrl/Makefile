# SPDX-License-Identifier: MIT
# pinctrl for the target, and a host build without hardware access for the
# tests: make test.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra
HOSTCC ?= cc
SRCS = pinctrl.c esp32s31.c signals.c board.c
HDRS = esp32s31.h board.h

pinctrl: $(SRCS) live.c $(HDRS)
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) live.c

pinctrl-host: $(SRCS) live-none.c $(HDRS)
	$(HOSTCC) $(WFLAGS) -Werror -O2 -o $@ $(SRCS) live-none.c

test: pinctrl-host
	./tests/run.sh ./pinctrl-host

clean:
	rm -f pinctrl pinctrl-host

.PHONY: test clean
