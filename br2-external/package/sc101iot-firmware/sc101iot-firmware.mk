################################################################################
#
# sc101iot-firmware
#
################################################################################

SC101IOT_FIRMWARE_VERSION = cd6194c97c43d6997eee13faf9e4c2ac4cae0c26
SC101IOT_FIRMWARE_SITE = $(call github,espressif,esp-video-components,$(SC101IOT_FIRMWARE_VERSION))
SC101IOT_FIRMWARE_LICENSE = Apache-2.0
SC101IOT_FIRMWARE_LICENSE_FILES = esp_cam_sensor/LICENSE
SC101IOT_FIRMWARE_DEPENDENCIES = host-python3

# Keep the vendor's mode data and license separate from the kernel driver.
define SC101IOT_FIRMWARE_BUILD_CMDS
	$(HOST_DIR)/bin/python3 $(SC101IOT_FIRMWARE_PKGDIR)/pack-mode.py \
		$(@D)/esp_cam_sensor/sensors/sc101iot/private_include/sc101iot_dvp_8bit_20Minput_640x480_yuv422_uyvy_15fps.h \
		$(@D)/vga-uyvy.bin
endef

define SC101IOT_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/vga-uyvy.bin \
		$(TARGET_DIR)/lib/firmware/sc101iot/vga-uyvy.bin
	$(INSTALL) -D -m 0644 $(@D)/esp_cam_sensor/LICENSE \
		$(TARGET_DIR)/usr/share/licenses/sc101iot-firmware/LICENSE
endef

$(eval $(generic-package))
