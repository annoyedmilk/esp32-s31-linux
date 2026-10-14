#!/bin/sh
# SPDX-License-Identifier: MIT
# Apply the ROM patch of ESP-Hosted to ESP-IDF, as a local commit on the
# commit in bootloader/esp-idf.version.  The ESP-Hosted Wi-Fi libraries need
# it.  A commit keeps the ESP-IDF version free of "-dirty".  Its name and
# date are fixed, so the commit, and the version, are the same on each host.
set -eu

cd "$(dirname "$0")/.."
: "${IDF_PATH:=$HOME/esp/esp-idf}"
patch=$PWD/external/esp-hosted-linux/esp/esp_driver/lib/rom.patch
idf() { git -C "$IDF_PATH" "$@"; }

files=$(idf apply --recount --numstat "$patch" | cut -f3)
idf apply --recount --reverse --check "$patch" 2>/dev/null ||
	idf apply --recount "$patch"
idf diff --quiet HEAD -- $files && exit 0
GIT_AUTHOR_DATE="@0 +0000" GIT_COMMITTER_DATE="@0 +0000" \
	idf -c user.name=esp32s31-linux -c user.email=esp32s31-linux@localhost \
	commit -q -m "Apply the ESP-Hosted ROM patch" -- $files
