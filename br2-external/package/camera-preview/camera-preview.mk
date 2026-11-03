################################################################################
#
# camera-preview
#
################################################################################

CAMERA_PREVIEW_VERSION = 1
CAMERA_PREVIEW_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../applications/camera-preview
CAMERA_PREVIEW_SITE_METHOD = local
CAMERA_PREVIEW_LICENSE = MIT
CAMERA_PREVIEW_LICENSE_FILES = LICENSE

define CAMERA_PREVIEW_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define CAMERA_PREVIEW_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr install
endef

$(eval $(generic-package))
