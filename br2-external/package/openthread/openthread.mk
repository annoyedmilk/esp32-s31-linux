################################################################################
#
# openthread
#
################################################################################

# The monthly tag.  mbedtls is a submodule, so this is a git download.
OPENTHREAD_VERSION = v2026.10.0
OPENTHREAD_SITE = https://github.com/openthread/openthread.git
OPENTHREAD_SITE_METHOD = git
OPENTHREAD_GIT_SUBMODULES = YES
OPENTHREAD_LICENSE = BSD-3-Clause
OPENTHREAD_LICENSE_FILES = LICENSE
OPENTHREAD_DEPENDENCIES = host-pkgconf

# ot-daemon as a full Thread device and border router on a radio
# co-processor, with the SRP and DNS-SD servers that Matter devices need,
# and ot-ctl.  Static libraries: shared ones make a cycle between the FTD
# and TCP libraries, which CMake rejects.
OPENTHREAD_CONF_OPTS = \
	-DBUILD_SHARED_LIBS=OFF \
	-DOT_PLATFORM=posix \
	-DOT_DAEMON=ON \
	-DOT_FTD=ON \
	-DOT_MTD=OFF \
	-DOT_RCP=OFF \
	-DOT_APP_CLI=OFF \
	-DOT_APP_NCP=OFF \
	-DOT_APP_RCP=OFF \
	-DOT_READLINE=OFF \
	-DOT_BORDER_ROUTER=ON \
	-DOT_BORDER_ROUTING=ON \
	-DOT_SERVICE=ON \
	-DOT_NETDATA_PUBLISHER=ON \
	-DOT_SRP_SERVER=ON \
	-DOT_DNSSD_SERVER=ON \
	-DOT_ECDSA=ON \
	-DOT_SLAAC=ON \
	-DOT_POSIX_SETTINGS_PATH=/var/lib/thread

$(eval $(cmake-package))
