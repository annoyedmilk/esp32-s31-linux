include $(sort $(wildcard $(BR2_EXTERNAL_ESP32S31_PATH)/package/*/*.mk))

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
