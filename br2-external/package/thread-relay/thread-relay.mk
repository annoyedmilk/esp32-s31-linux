################################################################################
#
# thread-relay
#
################################################################################

# The relay in tools/thread-relay of this repository.
THREAD_RELAY_VERSION = local
THREAD_RELAY_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../tools/thread-relay
THREAD_RELAY_SITE_METHOD = local
THREAD_RELAY_LICENSE = MIT

define THREAD_RELAY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" thread-relay
endef

define THREAD_RELAY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/thread-relay $(TARGET_DIR)/usr/sbin/thread-relay
endef

$(eval $(generic-package))
