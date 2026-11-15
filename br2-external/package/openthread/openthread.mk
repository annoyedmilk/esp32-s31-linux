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

# ot-daemon and ot-ctl: a Thread border router for Matter on the RCP.
# Static libraries: shared ones make a cycle that CMake rejects.
OPENTHREAD_CONF_OPTS = \
	-DBUILD_SHARED_LIBS=OFF \
	-DOT_PLATFORM=posix \
	-DOT_DAEMON=ON \
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
	-DOT_MDNS=ON \
	-DOT_SRP_ADV_PROXY=ON \
	-DOT_BORDER_AGENT=ON \
	-DOT_COMMISSIONER=ON \
	-DOT_NAT64_TRANSLATOR=ON \
	-DOT_NAT64_BORDER_ROUTING=ON \
	-DOT_DNS_UPSTREAM_QUERY=ON \
	-DOT_BORDER_AGENT_ID=ON \
	-DOT_BORDER_AGENT_MESHCOP_SERVICE=ON \
	-DOT_BORDER_AGENT_SERVICE_NAME="ESP32-S31 Linux" \
	-DOT_VENDOR_NAME="ESP32-S31 Linux" \
	-DOT_VENDOR_MODEL="ESP32-S31" \
	-DOT_POSIX_SETTINGS_PATH=/var/lib/thread \
	-DOT_PROJECT_CONFIG=$(OPENTHREAD_PKGDIR)/openthread-project-config.h

$(eval $(cmake-package))
