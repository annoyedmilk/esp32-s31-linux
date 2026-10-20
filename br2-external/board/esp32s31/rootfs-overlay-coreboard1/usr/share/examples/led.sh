#!/bin/sh
# SPDX-License-Identifier: MIT
# Set the color of the RGB LED (GPIO60), or show red, green and blue.
#
#   led.sh [RED GREEN BLUE]     each value is 0 to 255

LED=/sys/class/leds/rgb:status

color() {
	echo "$1 $2 $3" > $LED/multi_intensity && echo 255 > $LED/brightness
}

if [ $# -eq 3 ]; then
	color "$@"
	exit
fi
for rgb in "255 0 0" "0 255 0" "0 0 255" "0 0 0"; do
	color $rgb
	sleep 1
done
