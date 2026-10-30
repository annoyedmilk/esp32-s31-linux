################################################################################
#
# bmi270-firmware
#
################################################################################

BMI270_FIRMWARE_VERSION = 41129fcfe39c583ee5462d79195741945d51c1fe
BMI270_FIRMWARE_SITE = $(call github,boschsensortec,BMI270_SensorAPI,$(BMI270_FIRMWARE_VERSION))
BMI270_FIRMWARE_LICENSE = BSD-3-Clause
BMI270_FIRMWARE_LICENSE_FILES = LICENSE
BMI270_FIRMWARE_DEPENDENCIES = host-python3

define BMI270_FIRMWARE_BUILD_CMDS
	$(HOST_DIR)/bin/python3 $(BMI270_FIRMWARE_PKGDIR)/pack-firmware.py \
		$(@D)/bmi270.c $(@D)/bmi270-init-data.fw
endef

define BMI270_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/bmi270-init-data.fw \
		$(TARGET_DIR)/lib/firmware/bmi270-init-data.fw
	$(INSTALL) -D -m 0644 $(@D)/LICENSE \
		$(TARGET_DIR)/usr/share/licenses/bmi270-firmware/LICENSE
endef

$(eval $(generic-package))
