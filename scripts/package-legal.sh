#!/bin/sh
# SPDX-License-Identifier: MIT
# The licences and the sources of a release, next to its images.
#
#   scripts/package-legal.sh collect BOARD    after scripts/ci-build.sh:
#       the legal-info of Buildroot and the licence files of the loader,
#       in build/legal/BOARD/
#   scripts/package-legal.sh bundle VERSION DEST    with build/legal/* of
#       all boards: esp32s31-linux-VERSION-licenses.tar.xz and
#       esp32s31-linux-VERSION-sources.tar in DEST
set -eu

cd "$(dirname "$0")/.."
: "${IDF_PATH:=$HOME/esp/esp-idf}"
# The downloads of the build, as ci-build.sh sets them.
: "${BR_DL_DIR:=$PWD/build/br-dl}"
export BR2_DL_DIR="$BR_DL_DIR"

# Each warning of Buildroot needs a reason here, or the release stops.
# The source bundle has Buildroot; the root LICENSE covers ble-ha and
# thread-relay, whose licence text is not in their tool directory.
known='the Buildroot source code has not been saved
ble-ha-local: cannot save license (BLE_HA_LICENSE_FILES not defined)
thread-relay-local: cannot save license (THREAD_RELAY_LICENSE_FILES not defined)'

collect() {
	br=${BR_OUTPUT:-$PWD/build/br-output-$1}
	out=build/legal/$1
	make -C external/buildroot O="$br" BR2_EXTERNAL="$PWD/br2-external" legal-info
	new=$(sed -n 's/^WARNING: //p' "$br/legal-info/README" | grep -vxF "$known" || true)
	if [ -n "$new" ]; then
		printf 'legal-info has new warnings:\n%s\n' "$new" >&2
		exit 1
	fi
	rm -rf "$out"
	mkdir -p "$out/loader"
	cp -a "$br/legal-info/." "$out/buildroot"
	# The licence files of the components in the loader, and of ESP-IDF.
	cp "$IDF_PATH/LICENSE" "$IDF_PATH/docs/en/COPYRIGHT.rst" "$out/loader/"
	python3 -c 'import json, sys
for c, i in json.load(open(sys.argv[1]))["build_component_info"].items():
	print(c, i["dir"])' "build/$1/bootloader/project_description.json" |
	while read -r name dir; do
		(cd "$dir" && find . -maxdepth 3 -type f \( -iname 'LICENSE*' -o \
			-iname 'COPYING*' -o -iname 'NOTICE*' \) ! -path '*/test*') |
		while read -r f; do
			mkdir -p "$out/loader/$name/${f%/*}"
			cp "$dir/$f" "$out/loader/$name/$f"
		done
	done
}

# SRC DST: copy the files of SRC into DST; a file of both must be the same.
merge() {
	[ -d "$1" ] || return 0
	(cd "$1" && find . -type f) | while read -r f; do
		if [ -e "$2/$f" ]; then
			cmp -s "$1/$f" "$2/$f" || { echo "$f differs between the boards" >&2; exit 1; }
		else
			mkdir -p "$2/${f%/*}"
			cp "$1/$f" "$2/$f"
		fi
	done
}

readme() {
	kernel=$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="\(.*\)"/\1/p' \
		br2-external/configs/esp32s31_defconfig)
	cat <<TXT
ESP32-S31 Linux $1: licences and sources

The images of this release contain free software.  This bundle has the
licence texts and the lists of the packages; esp32s31-linux-$1-sources.tar
of the same release has the sources.

- The kernel (Image and the device tree): Linux $kernel with the files and
  patches of linux/ and the ESP-Hosted host driver with its patches in
  br2-external/package/esp-hosted-linux/ (repo/ of the source bundle).
- The root file system and the disk images: the packages of
  buildroot/BOARD/manifest.csv, their texts in licenses/.  The GCC runtime
  libraries come from the host packages of host-manifest.csv, their texts
  in host-licenses/.  Buildroot itself is in repo/external/buildroot.
- The loader: ESP-IDF $(cat bootloader/esp-idf.version) with
  bootloader/idf-patches/, the code of bootloader/ and the ESP-Hosted
  firmware.  The licence files of its components are in loader/.  The
  Wi-Fi libraries of esp/esp_driver/lib/esp32s31 come unchanged from
  external/esp-hosted-linux; that repository has no licence statement for
  them (see esp-hosted-linux/).
- OpenSBI: opensbi/.
- LICENSE (MIT): the build system, the scripts, the files of the root and
  the tools ble-ha, thread-relay and pinctrl; tools/pinctrl/pinctrl.c is
  BSD-3-Clause (see its header).  A patch has the licence of the software
  that it changes.

To install a changed image, build it from repo/ (README.md) and flash it
with the commands of FLASH.txt.  The images do not use secure boot or flash
encryption, so the chip takes any image.
TXT
}

bundle() {
	name=esp32s31-linux-$1
	lic=build/bundle/$name-licenses
	src=build/bundle/$name-sources
	rm -rf build/bundle
	mkdir -p "$lic/buildroot" "$lic/esp-hosted-linux" "$lic/opensbi" "$src/repo"
	for b in build/legal/*; do
		mkdir -p "$lic/buildroot/${b##*/}"
		for f in README manifest.csv host-manifest.csv buildroot.config; do
			cp "$b/buildroot/$f" "$lic/buildroot/${b##*/}/"
		done
		merge "$b/buildroot/licenses" "$lic/licenses"
		merge "$b/buildroot/host-licenses" "$lic/host-licenses"
		merge "$b/loader" "$lic/loader"
		merge "$b/buildroot/sources" "$src/sources"
		merge "$b/buildroot/host-sources" "$src/host-sources"
	done
	cp LICENSE "$lic/"
	cp external/buildroot/COPYING "$lic/buildroot/"
	cp -R external/esp-hosted-linux/LICENSE external/esp-hosted-linux/LICENSES \
		"$lic/esp-hosted-linux/"
	cp external/opensbi/COPYING.BSD external/opensbi/ThirdPartyNotices.md "$lic/opensbi/"
	readme "$1" > "$lic/README"
	git ls-files --recurse-submodules -z | tar --null -T - -cf - | tar -C "$src/repo" -xf -
	tar -C build/bundle -cJf "$2/$name-licenses.tar.xz" "$name-licenses"
	tar -C build/bundle -cf "$2/$name-sources.tar" "$name-sources"
	ls -l "$2/$name-licenses.tar.xz" "$2/$name-sources.tar"
}

case ${1:-} in
collect) collect "${2:?usage: $0 collect BOARD}" ;;
bundle) bundle "${2:?usage: $0 bundle VERSION DEST}" "${3:?usage: $0 bundle VERSION DEST}" ;;
*) echo "usage: $0 collect BOARD | bundle VERSION DEST" >&2; exit 2 ;;
esac
