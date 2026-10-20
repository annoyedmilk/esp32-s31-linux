# SPDX-License-Identifier: MIT
# Example for /mnt/usb/autostart.sh.  The board runs that file at each boot,
# as root.  Its output is in /tmp/autostart.log.

# A dim green LED shows that the boot is complete.
/usr/share/examples/led.sh 0 32 0

# The web page of the examples on port 8080: the LED and the free pins of
# J2.  It has no password, so start it only on a network that you trust.
#httpd -p 8080 -h /usr/share/examples/www
