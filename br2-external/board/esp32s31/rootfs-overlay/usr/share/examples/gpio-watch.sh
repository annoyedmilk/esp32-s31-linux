#!/bin/sh
# SPDX-License-Identifier: MIT
# Print each edge of a GPIO with the time, from the interrupt of the pin.
# Make sure that the pin is free on your board.  Ctrl-C ends it.
#
#   gpio-watch.sh PIN [rising|falling|both]

[ $# -eq 1 ] || [ $# -eq 2 ] || {
	echo "usage: $0 PIN [rising|falling|both]" >&2
	exit 1
}
exec gpiomon -c gpiochip0 -e "${2:-both}" -E realtime --localtime \
	-F '%U %E' "$1"
