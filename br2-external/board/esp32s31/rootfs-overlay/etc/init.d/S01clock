#!/bin/sh
# SPDX-License-Identifier: MIT
# No RTC: set the last known time, so TLS works before NTP.

STAMP=/etc/timestamp
FORMAT='%Y-%m-%d %H:%M:%S'

case "$1" in
start)
	[ -r "$STAMP" ] || exit 0
	saved=$(cat "$STAMP")
	# Only move forward, so this cannot undo an NTP sync.
	if [ "$(date -u +%s)" -lt "$(date -u -d "$saved" +%s 2>/dev/null || echo 0)" ]
	then
		date -u -s "$saved" >/dev/null && echo "clock: restored $saved UTC"
	fi
	;;
stop)
	# Save only a correct clock.
	[ "$(date -u +%Y)" -ge 2020 ] && date -u +"$FORMAT" >"$STAMP"
	;;
esac

exit 0
