# SPDX-License-Identifier: MIT
# pinctrl for the target.

CFLAGS ?= -Os
# Not in CFLAGS: a CFLAGS on the command line replaces those of this file.
WFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra
SRCS = pinctrl.c esp32s31.c signals.c board.c live.c

pinctrl: $(SRCS) esp32s31.h board.h
	$(CC) $(WFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f pinctrl

.PHONY: clean
