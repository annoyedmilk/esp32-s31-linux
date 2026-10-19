#!/bin/sh
# SPDX-License-Identifier: MIT
# Read a GPIO, or set it.  Make sure that the pin is free on your board.
#
#   gpio.sh PIN          print 0 or 1
#   gpio.sh PIN 0|1      set the pin and hold it until Ctrl-C

if [ $# -eq 1 ]; then
	exec gpioget --numeric -c gpiochip0 "$1"
elif [ $# -eq 2 ]; then
	exec gpioset -c gpiochip0 "$1=$2"
fi
echo "usage: $0 PIN [0|1]" >&2
exit 1
