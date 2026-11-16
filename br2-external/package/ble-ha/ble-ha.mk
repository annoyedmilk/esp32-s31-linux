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

# -B: the local site copies the tool directory with a binary from the host.
define BLE_HA_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) $(TARGET_CONFIGURE_OPTS) ble-ha
endef

define BLE_HA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ble-ha $(TARGET_DIR)/usr/sbin/ble-ha
endef

$(eval $(generic-package))
