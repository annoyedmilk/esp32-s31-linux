#!/bin/sh
# SPDX-License-Identifier: MIT
# Print the Python interpreter for this checkout's ESP-IDF version.
set -eu

cd "$(dirname "$0")/.."
: "${IDF_PATH:=$PWD/external/esp-idf}"
: "${IDF_TOOLS_PATH:=$HOME/.espressif}"

if [ -n "${PYTHON:-}" ]; then
	python=$(command -v "$PYTHON") || {
		echo 'PYTHON must name an executable interpreter, without arguments' >&2
		exit 1
	}
elif [ -n "${IDF_PYTHON_ENV_PATH:-}" ]; then
	python=$IDF_PYTHON_ENV_PATH/bin/python
else
	version=$(sed -n 's/^set(IDF_VERSION_M[AI][JN]OR \([0-9]*\))/\1/p' \
		"$IDF_PATH/tools/cmake/version.cmake" | paste -sd. -)
	[ -n "$version" ] || {
		echo "Cannot read the ESP-IDF version from $IDF_PATH" >&2
		exit 1
	}
	python=
	for candidate in "$IDF_TOOLS_PATH"/python_env/idf"${version}"_*/bin/python; do
		[ -x "$candidate" ] || continue
		python=$candidate
	done
fi

if [ -z "$python" ] || [ ! -x "$python" ]; then
	echo 'No ESP-IDF Python environment found; run install.sh for this SDK,' >&2
	echo 'activate its environment, or set PYTHON / IDF_PYTHON_ENV_PATH explicitly' >&2
	exit 1
fi
case $python in
/*) ;;
*) python=$PWD/$python ;;
esac
printf '%s\n' "$python"
