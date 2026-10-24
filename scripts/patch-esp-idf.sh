#!/bin/sh
# SPDX-License-Identifier: MIT
# Apply the ESP-IDF patch of ESP-Hosted (the ROM patch for its Wi-Fi
# libraries) and the backported fixes in bootloader/idf-patches as one local
# commit on the commit in bootloader/esp-idf.version.  A commit keeps the
# ESP-IDF version free of "-dirty".  Its name and date are fixed, so the
# commit, and the version, are the same on each host.  An earlier commit of
# this script is replaced.
set -eu

cd "$(dirname "$0")/.."
: "${IDF_PATH:=$HOME/esp/esp-idf}"
lib=$PWD/external/esp-hosted-linux/esp/esp_driver/lib
patches="$lib/rom.patch $PWD/bootloader/idf-patches/*.patch"
idf() { git -C "$IDF_PATH" "$@"; }

if [ "$(idf rev-parse HEAD~1 2>/dev/null)" = "$(cat bootloader/esp-idf.version)" ] &&
	[ "$(idf log -1 --format=%an)" = esp32s31-linux ]; then
	idf reset -q --soft HEAD~1
fi

files=
for patch in $patches; do
	files="$files $(idf apply --recount --numstat "$patch" | cut -f3)"
	idf apply --recount --reverse --check "$patch" 2>/dev/null ||
		idf apply --recount "$patch"
done
idf add -- $files
idf diff --cached --quiet HEAD -- $files && exit 0
GIT_AUTHOR_DATE="@0 +0000" GIT_COMMITTER_DATE="@0 +0000" \
	idf -c user.name=esp32s31-linux -c user.email=esp32s31-linux@localhost \
	commit -q -m "Apply the ESP-Hosted patches and the backports" -- $files
