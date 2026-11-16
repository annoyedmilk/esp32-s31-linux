#!/bin/sh
# SPDX-License-Identifier: MIT
# No RTC: set the last known time, so TLS works before NTP.
# rcS sources an S*.sh script in a subshell, with $1 = start: no new shell
# starts.  Each other script runs as its own process.

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
stop|save)
	# Save only a correct clock.  A new file and a rename: a reset that
	# comes before the data is on the disk keeps the old file.
	[ "$(date -u +%Y)" -ge 2020 ] && date -u +"$FORMAT" >"$STAMP.new" &&
		mv "$STAMP.new" "$STAMP"
	# NTP has set the clock: S42wireguard waits for this.
	[ "$1" = save ] && : >/var/run/clock-set
	;;
esac

exit 0
