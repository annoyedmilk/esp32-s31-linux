################################################################################
#
# esp32s31-pinctrl
#
################################################################################

# The pinctrl tool in tools/pinctrl of this repository.
ESP32S31_PINCTRL_VERSION = local
ESP32S31_PINCTRL_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../tools/pinctrl
ESP32S31_PINCTRL_SITE_METHOD = local
ESP32S31_PINCTRL_LICENSE = BSD-3-Clause (pinctrl.c), MIT
ESP32S31_PINCTRL_LICENSE_FILES = pinctrl.c

define ESP32S31_PINCTRL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" pinctrl
endef

define ESP32S31_PINCTRL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pinctrl $(TARGET_DIR)/usr/bin/pinctrl
endef

$(eval $(generic-package))
