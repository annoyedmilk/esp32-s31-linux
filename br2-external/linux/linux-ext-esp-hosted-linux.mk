################################################################################
#
# Copy the ESP-Hosted host driver into the kernel tree
#
################################################################################

LINUX_EXTENSIONS += esp-hosted-linux

define ESP_HOSTED_LINUX_PREPARE_KERNEL
	mkdir -p $(LINUX_DIR)/drivers/net/wireless/espressif/esp_hosted
	cp -R $(ESP_HOSTED_LINUX_DIR)/host/*.c $(ESP_HOSTED_LINUX_DIR)/host/include \
		$(BR2_EXTERNAL_ESP32S31_PATH)/../shared/esp32s31-hosted-shm.h \
		$(LINUX_DIR)/drivers/net/wireless/espressif/esp_hosted/
endef
