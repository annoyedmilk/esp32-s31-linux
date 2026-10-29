include $(sort $(wildcard $(BR2_EXTERNAL_ESP32S31_PATH)/package/*/*.mk))

# Copy linux/ (without patches/) into the kernel before the patches add it.
define ESP32S31_COPY_KERNEL_SOURCES
	tar -C $(BR2_EXTERNAL_ESP32S31_PATH)/../linux --exclude=./patches \
		--exclude='._*' --exclude=.DS_Store -cf - . | tar -C $(LINUX_DIR) -xf -
endef
LINUX_PRE_PATCH_HOOKS += ESP32S31_COPY_KERNEL_SOURCES

# RISC-V modules retain many unused assembler labels after --strip-debug.
# Keep relocation targets, but omit unused symbols from installed modules.
ifeq ($(BR2_STRIP_strip),y)
LINUX_MAKE_FLAGS += INSTALL_MOD_STRIP=--strip-unneeded
endif

# After Buildroot writes .config: drop the EAP methods, DPP, P2P and TDLS
# that nothing here needs.
WPA_SUPPLICANT_UNUSED = EAP_SIM EAP_AKA EAP_AKA_PRIME EAP_SAKE EAP_GPSK \
	EAP_GPSK_SHA256 EAP_TNC EAP_IKEV2 EAP_EKE EAP_PAX EAP_PWD EAP_TEAP \
	EAP_FAST EAP_LEAP EAP_OTP EAP_PSK DPP DPP2 DPP3 P2P TDLS
define WPA_SUPPLICANT_DISABLE_UNUSED
	sed -i $(patsubst %,-e 's/^CONFIG_%=/#CONFIG_%=/',$(WPA_SUPPLICANT_UNUSED)) \
		$(WPA_SUPPLICANT_CONFIG)
endef
WPA_SUPPLICANT_POST_CONFIGURE_HOOKS += WPA_SUPPLICANT_DISABLE_UNUSED

# No glib bindings or gpio-manager: unused, and they keep libgio in the image.
LIBGPIOD2_CONF_OPTS += -Dbindings-glib=disabled -Ddbus=disabled

# Without OpenSSL, iperf3 does not load libcrypto at each start.
IPERF3_CONF_OPTS += --without-openssl

# pcre2 always installs pcre2grep and pcre2test; only the library is used.
define PCRE2_REMOVE_PROGRAMS
	rm -f $(TARGET_DIR)/usr/bin/pcre2grep $(TARGET_DIR)/usr/bin/pcre2test
endef
PCRE2_POST_INSTALL_TARGET_HOOKS += PCRE2_REMOVE_PROGRAMS
