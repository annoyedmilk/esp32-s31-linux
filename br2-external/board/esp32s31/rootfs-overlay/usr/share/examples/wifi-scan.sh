#!/bin/sh
# SPDX-License-Identifier: MIT
# List the Wi-Fi networks in range, the strongest first: signal, channel and
# name.  The scan takes a few seconds.
#
#   wifi-scan.sh

iw dev wlan0 scan | awk '
function show() { if (bss) printf "%4d dBm  ch %3d  %s\n", signal, channel, ssid }
/^BSS / { show(); bss = 1; ssid = "(hidden)" }
/^\tfreq:/ { f = int($2); channel = f == 2484 ? 14 : f < 2484 ? (f - 2407) / 5 : (f - 5000) / 5 }
/^\tsignal:/ { signal = $2 }
/^\tSSID: ./ { ssid = substr($0, 8) }
END { show() }' | sort -rn
