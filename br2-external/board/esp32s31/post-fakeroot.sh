#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs on the copy of target/ that the images are made from: target/ itself
# stays complete for the next build.
set -eu

# Keep only the CA bundle (curl reads it) and make it the OpenSSL default file.
rm -rf "$1/usr/share/ca-certificates"
find "$1/etc/ssl/certs" -type l -delete
ln -sf certs/ca-certificates.crt "$1/etc/ssl/cert.pem"

# Remove the glib2 tools, then each listed library that no ELF file needs.
# A library comes before the ones it needs.  dlopen users are not listed.
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
