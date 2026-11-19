#!/bin/sh
# SPDX-License-Identifier: MIT
# Usage: sh scripts/enable-amoled.sh BUILDROOT_OUTPUT
# Run after coreboard1_defconfig and before building. A clean kernel extract
# copies the repository sources and applies the new framebuffer patch.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "${1:-}" in
"") echo "usage: sh scripts/enable-amoled.sh BUILDROOT_OUTPUT" >&2; exit 2 ;;
esac
out=$(CDPATH= cd -- "$1" && pwd)
test -f "$out/.config" || { echo "run coreboard1_defconfig first" >&2; exit 1; }
grep -q 'linux-coreboard1.fragment' "$out/.config" ||
	{ echo "output must be configured for coreboard1" >&2; exit 1; }
if ! grep -q 'linux-amoled.fragment' "$out/.config"; then
	sed -i '/^BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES=/s@"$@ $(BR2_EXTERNAL_ESP32S31_PATH)/board/esp32s31/linux-amoled.fragment"@' "$out/.config"
fi
make -C "$repo/external/buildroot" O="$out" BR2_EXTERNAL="$repo/br2-external" olddefconfig
make -C "$repo/external/buildroot" O="$out" BR2_EXTERNAL="$repo/br2-external" linux-dirclean
echo "AMOLED console enabled. Build this output without reapplying coreboard1_defconfig."
