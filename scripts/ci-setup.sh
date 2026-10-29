#!/bin/sh
# SPDX-License-Identifier: MIT
# Install the host packages and ESP-IDF for scripts/ci-build.sh, on Ubuntu
# 24.04.  The release workflow uses it.
set -eu

cd "$(dirname "$0")/.."
sudo=
[ "$(id -u)" = 0 ] || sudo=sudo

$sudo apt-get update
DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y --no-install-recommends \
	bc binutils build-essential bzip2 ca-certificates cpio diffutils \
	dosfstools file findutils git gzip libncurses-dev make mtools patch \
	perl python3 python3-pip python3-venv rsync sed tar unzip wget \
	xz-utils zip cmake ninja-build flex bison gperf ccache libffi-dev \
	libssl-dev libusb-1.0-0

# ESP-IDF at the tested commit, only with the tools for this chip.
: "${IDF_PATH:=$PWD/external/esp-idf}"
commit=$(cat bootloader/esp-idf.version)
idf() { git -C "$IDF_PATH" "$@"; }
if [ ! -e "$IDF_PATH/.git" ]; then
	git clone --filter=blob:none https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
idf cat-file -e "$commit^{commit}" 2>/dev/null ||
	idf fetch --filter=blob:none origin "$commit"
idf checkout -q -f --detach "$commit"
idf submodule update --init --recursive --filter=blob:none
IDF_PATH=$IDF_PATH scripts/patch-esp-idf.sh
"$IDF_PATH/install.sh" esp32s31
# The tested esptool, in the Python environment of ESP-IDF.
python=$(ls -d "$HOME"/.espressif/python_env/*/bin/python | tail -1)
"$python" -m pip install "esptool==$(cat bootloader/esptool.version)"
