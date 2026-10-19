#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs on the copy of the target directory that each image is made from.
# A change here does not touch target/, so the next build starts from the
# complete tree.
set -eu

# ca-certificates installs each certificate twice: as a file with a link in
# /etc/ssl/certs, and in the bundle, which curl reads.  Keep the bundle, and
# make it the default file of OpenSSL for the other programs.  The bundle is
# made from the single files at each build, so they must stay in target/.
rm -rf "$1/usr/share/ca-certificates"
find "$1/etc/ssl/certs" -type l -delete
ln -sf certs/ca-certificates.crt "$1/etc/ssl/cert.pem"

# Packages install libraries that nothing here uses: glib2 five next to
# libglib (and its tools), libnl, ncurses and readline their extra parts,
# BlueZ a libbluetooth that its programs do not link.  Remove the glib2
# tools, then each library that no ELF file in the image needs.  A library
# comes before the ones it needs.  OpenSSL modules and libgcc_s are not on
# the list: programs load them with dlopen.
readelf --version >/dev/null
cd "$1"
rm -f usr/bin/gapplication usr/bin/gdbus usr/bin/gi-compile-repository \
	usr/bin/gi-decompile-typelib usr/bin/gi-inspect-typelib usr/bin/gio \
	usr/bin/gio-querymodules usr/bin/gresource usr/bin/gsettings \
	usr/libexec/gio-launch-desktop
rm -rf usr/lib/gio usr/share/glib-2.0
for lib in libgirepository-2.0 libgio-2.0 libgobject-2.0 libgmodule-2.0 \
	libgthread-2.0 libffi libnl-nf-3 libnl-xfrm-3 libnl-idiag-3 libform \
	libmenu libpanel libhistory libbluetooth; do
	for f in usr/lib/"$lib".so.*; do
		[ -f "$f" ] && [ ! -L "$f" ] || continue
		so=$(readelf -d "$f" | sed -n 's/.*Library soname: \[\(.*\)\]/\1/p')
		find . -type f -exec readelf -d {} + 2>/dev/null |
			grep -q "Shared library: \[$so\]" || rm -f "usr/lib/$lib".so*
	done
done
