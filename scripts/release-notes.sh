#!/bin/sh
# SPDX-License-Identifier: MIT
# Print the GitHub release text of a tag: its section of CHANGELOG.md, one
# line for each bullet (GitHub breaks the text at each newline).  A release
# candidate takes the section of its version.  No section: an error.
#
#   scripts/release-notes.sh v1.0.0-rc1
set -eu

cd "$(dirname "$0")/.."
tag=${1:?usage: $0 TAG}
version=${tag%%-*}
section=$(awk -v h="## $version" '$0 == h { f = 1; next } /^## / { f = 0 } f' CHANGELOG.md |
	awk '/^  [^ ]/ { sub(/^ +/, " "); line = line $0; next }
		{ if (n++) print line; line = $0 } END { print line }')
if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
	echo "CHANGELOG.md has no section $version" >&2
	exit 1
fi
printf '%s\n' "$section" | sed '/^$/d'
printf '\n%s\n\n%s\n' \
	"The boards, the quick start and the update are in the [README](https://github.com/annoyedmilk/esp32-s31-linux/blob/$tag/README.md)." \
	'The `*-licenses.tar.xz` and `*-sources.tar` files have the licence texts and the sources of the images.'
