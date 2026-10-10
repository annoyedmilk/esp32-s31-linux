# SPDX-License-Identifier: MIT
# Example for /mnt/sd/autostart.sh.  The board runs that file at each boot,
# as root, after the card and Wi-Fi start.  Copy this file there and change
# it.  Its output is in /tmp/autostart.log.

# A dim green LED shows that the boot is complete.
micropython /usr/share/examples/led.py 0 32 0
