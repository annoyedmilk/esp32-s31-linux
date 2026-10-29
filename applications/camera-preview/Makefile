# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS ?= -O2
PREFIX ?= /usr

.PHONY: all install clean
all: camera-preview

camera-preview: camera-preview.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra $(LDFLAGS) -o $@ $< $(LDLIBS)

install: camera-preview
	install -D -m 0755 camera-preview $(DESTDIR)$(PREFIX)/bin/camera-preview

clean:
	rm -f camera-preview
