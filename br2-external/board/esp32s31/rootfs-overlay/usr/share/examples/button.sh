#!/bin/sh
# SPDX-License-Identifier: MIT
# Run a command at each press of a button between a GPIO and GND.  The pull
# up of the pin holds it high, and a press pulls it low.  The kernel
# ignores the bounces of the contact for 20 ms (DEBOUNCE).  Make sure that
# the pin is free on your board.  For example, toggle the LED:
#
#   button.sh 47 'f=/sys/class/leds/rgb:status/brightness; [ $(cat $f) = 0 ] && echo 255 > $f || echo 0 > $f'
#
#   button.sh PIN COMMAND

DEBOUNCE=${DEBOUNCE:-20ms}

[ $# -eq 2 ] || { echo "usage: $0 PIN COMMAND" >&2; exit 1; }
gpiomon -c gpiochip0 -e falling -b pull-up -p "$DEBOUNCE" -F '%e' "$1" |
while read -r _; do
	sh -c "$2"
done
