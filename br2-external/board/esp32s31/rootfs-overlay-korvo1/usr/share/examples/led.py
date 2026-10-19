# SPDX-License-Identifier: MIT
# Set the color of the RGB LED, or show red, green and blue.
#
#   micropython led.py [RED GREEN BLUE]     each value is 0 to 255
import sys
import time

LED = "/sys/class/leds/rgb:status/"


def write(name, value):
    with open(LED + name, "w") as f:
        f.write(value)


def color(red, green, blue):
    write("multi_intensity", "%d %d %d" % (red, green, blue))
    write("brightness", "255")


if len(sys.argv) == 4:
    color(*[int(arg) for arg in sys.argv[1:]])
else:
    for rgb in ((255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 0, 0)):
        color(*rgb)
        time.sleep(1)
