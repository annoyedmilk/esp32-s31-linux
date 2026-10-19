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
