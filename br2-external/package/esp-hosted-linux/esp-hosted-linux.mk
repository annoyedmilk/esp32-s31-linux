################################################################################
#
# esp-hosted-linux
#
################################################################################

# The host driver of the submodule.  The transport and its Kbuild come from
# this package.  Buildroot does not patch a local package, so the hook also
# applies the patches of this directory.
ESP_HOSTED_LINUX_VERSION = local
ESP_HOSTED_LINUX_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../external/esp-hosted-linux
ESP_HOSTED_LINUX_SITE_METHOD = local
ESP_HOSTED_LINUX_LICENSE = GPL-2.0
ESP_HOSTED_LINUX_LICENSE_FILES = LICENSES/GPL-2.0
ESP_HOSTED_LINUX_MODULE_SUBDIRS = host

define ESP_HOSTED_LINUX_ADD_SHM_TRANSPORT
	$(INSTALL) -m 0644 $(ESP_HOSTED_LINUX_PKGDIR)/shm/Kbuild \
		$(ESP_HOSTED_LINUX_PKGDIR)/shm/esp_shm.c $(@D)/host/
	$(INSTALL) -m 0644 \
		$(BR2_EXTERNAL_ESP32S31_PATH)/../shared/esp32s31-hosted-shm.h \
		$(@D)/host/include/
	$(APPLY_PATCHES) $(@D) $(ESP_HOSTED_LINUX_PKGDIR) \*.patch
endef
ESP_HOSTED_LINUX_POST_RSYNC_HOOKS += ESP_HOSTED_LINUX_ADD_SHM_TRANSPORT

$(eval $(kernel-module))
$(eval $(generic-package))
