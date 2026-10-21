include $(sort $(wildcard $(BR2_EXTERNAL_ESP32S31_PATH)/package/*/*.mk))

# The new kernel source files of the SoC are under linux/, next to
# linux/patches/.  Copy them into the kernel release before Buildroot
# applies the patches, which add them to Kconfig and the Makefiles.
define ESP32S31_COPY_KERNEL_SOURCES
	tar -C $(BR2_EXTERNAL_ESP32S31_PATH)/../linux --exclude=./patches \
		--exclude='._*' --exclude=.DS_Store -cf - . | tar -C $(LINUX_DIR) -xf -
endef
LINUX_PRE_PATCH_HOOKS += ESP32S31_COPY_KERNEL_SOURCES

# Buildroot's EAP option turns on each EAP method, and its WPA3 and AP
# options add DPP and P2P.  Turn off what Personal, WPA3 and Enterprise
# (PEAP, TTLS and TLS, as eduroam uses) do not need, for flash.  This runs
# after Buildroot writes .config, which would add these options again.
WPA_SUPPLICANT_UNUSED = EAP_SIM EAP_AKA EAP_AKA_PRIME EAP_SAKE EAP_GPSK \
	EAP_GPSK_SHA256 EAP_TNC EAP_IKEV2 EAP_EKE EAP_PAX EAP_PWD EAP_TEAP \
	EAP_FAST EAP_LEAP EAP_OTP EAP_PSK DPP DPP2 DPP3 P2P TDLS
define WPA_SUPPLICANT_DISABLE_UNUSED
	sed -i $(patsubst %,-e 's/^CONFIG_%=/#CONFIG_%=/',$(WPA_SUPPLICANT_UNUSED)) \
		$(WPA_SUPPLICANT_CONFIG)
endef
WPA_SUPPLICANT_POST_CONFIGURE_HOOKS += WPA_SUPPLICANT_DISABLE_UNUSED

# libgpiod builds its glib bindings and gpio-manager when glib is in the
# build (BlueZ brings it).  No program here uses them, and the bindings keep
# libgio, libgobject and libffi in the image.
LIBGPIOD2_CONF_OPTS += -Dbindings-glib=disabled -Ddbus=disabled

# iperf3 links OpenSSL for its rarely used authentication when OpenSSL is in
# the build, and then loads libcrypto at each start.
IPERF3_CONF_OPTS += --without-openssl
