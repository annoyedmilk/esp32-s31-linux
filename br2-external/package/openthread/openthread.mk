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
# and ot-ctl.  The mDNS responder and the SRP advertising proxy publish the
# services of the Thread devices on the backbone, where a Matter controller
# finds them; the border agent announces the board as _meshcop._udp.
# The commissioner role lets Thread devices join with a PSKd (ot-ctl
# commissioner); Matter devices get the dataset from the Matter controller.
# NAT64 and DNS upstream queries give Thread devices IPv4 hosts and names
# through the backbone; ot-daemon translates to 192.168.255.0/24, and
# nftables.conf masquerades that network.
# Static libraries: shared ones make a cycle between the FTD and TCP
# libraries, which CMake rejects.
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
	-DOT_POSIX_SETTINGS_PATH=/var/lib/thread

$(eval $(cmake-package))
