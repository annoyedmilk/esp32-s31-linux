# SPDX-License-Identifier: MIT
# The local time zone of /etc/default/tz, as a POSIX TZ string.

[ -r /etc/default/tz ] && . /etc/default/tz
[ -n "$TZ" ] && export TZ || unset TZ
