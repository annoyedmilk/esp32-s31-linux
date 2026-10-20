#!/bin/sh
# SPDX-License-Identifier: MIT
# Send the uptime and the free memory to a server with HTTPS.  curl checks
# the certificate.  To send each 5 minutes:
#
#   echo '*/5 * * * * /usr/share/examples/report.sh https://example.com/log' | crontab -

[ $# -eq 1 ] || { echo "usage: $0 URL" >&2; exit 1; }
up=$(cut -d' ' -f1 /proc/uptime)
mem=$(awk '$1 == "MemAvailable:" { print $2 }' /proc/meminfo)
exec curl -fsS -m 30 -d "host=$(hostname)&uptime=$up&memavailable_kb=$mem" "$1"
