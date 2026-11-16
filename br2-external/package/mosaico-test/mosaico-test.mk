################################################################################
#
# mosaico-test
#
################################################################################

MOSAICO_TEST_VERSION = 1
MOSAICO_TEST_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../applications/mosaico-test
MOSAICO_TEST_SITE_METHOD = local
MOSAICO_TEST_LICENSE = MIT
MOSAICO_TEST_LICENSE_FILES = LICENSE LVGL-LICENCE.txt
MOSAICO_TEST_DEPENDENCIES = alsa-lib camera-preview
MOSAICO_TEST_CONF_OPTS = -DLVGL_DIR=$(BR2_EXTERNAL_ESP32S31_PATH)/../external/lvgl

define MOSAICO_TEST_COPY_LVGL_LICENSE
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_ESP32S31_PATH)/../external/lvgl/LICENCE.txt \
		$(@D)/LVGL-LICENCE.txt
endef
MOSAICO_TEST_POST_RSYNC_HOOKS += MOSAICO_TEST_COPY_LVGL_LICENSE

define MOSAICO_TEST_INSTALL_LICENSES
	$(INSTALL) -D -m 0644 $(@D)/LICENSE \
		$(TARGET_DIR)/usr/share/licenses/mosaico-test/LICENSE
	$(INSTALL) -D -m 0644 $(@D)/LVGL-LICENCE.txt \
		$(TARGET_DIR)/usr/share/licenses/mosaico-test/LVGL-LICENCE.txt
endef
MOSAICO_TEST_POST_INSTALL_TARGET_HOOKS += MOSAICO_TEST_INSTALL_LICENSES

$(eval $(cmake-package))
