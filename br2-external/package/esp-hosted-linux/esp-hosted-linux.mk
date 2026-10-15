################################################################################
#
# esp-hosted-linux
#
################################################################################

# The host driver of the submodule, which the linux extension in
# br2-external/linux copies into the kernel.  Buildroot does not patch a
# local package, so the hook applies the patches of this directory.
ESP_HOSTED_LINUX_VERSION = local
ESP_HOSTED_LINUX_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/../external/esp-hosted-linux
ESP_HOSTED_LINUX_SITE_METHOD = local
ESP_HOSTED_LINUX_LICENSE = GPL-2.0
ESP_HOSTED_LINUX_LICENSE_FILES = LICENSES/GPL-2.0

define ESP_HOSTED_LINUX_APPLY_PATCHES
	$(APPLY_PATCHES) $(@D) $(ESP_HOSTED_LINUX_PKGDIR) \*.patch
endef
ESP_HOSTED_LINUX_POST_RSYNC_HOOKS += ESP_HOSTED_LINUX_APPLY_PATCHES

$(eval $(generic-package))
