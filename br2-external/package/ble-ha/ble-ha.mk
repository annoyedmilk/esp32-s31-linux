################################################################################
#
# ble-ha
#
################################################################################

# The program in tools/ble-ha of this repository.
BLE_HA_VERSION = local
BLE_HA_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../tools/ble-ha
BLE_HA_SITE_METHOD = local
BLE_HA_LICENSE = MIT

define BLE_HA_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" ble-ha
endef

define BLE_HA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ble-ha $(TARGET_DIR)/usr/sbin/ble-ha
endef

$(eval $(generic-package))
