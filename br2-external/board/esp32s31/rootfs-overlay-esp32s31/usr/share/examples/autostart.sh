# SPDX-License-Identifier: MIT
# Example for /root/autostart.sh.  The board runs that file at each boot,
# as root.  Its output is in /tmp/autostart.log.

# The GPIO web page of the examples on port 8080.
httpd -p 8080 -h /usr/share/examples/www
