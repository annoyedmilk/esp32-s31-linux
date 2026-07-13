idf_component_register(SRCS main.c
                       REQUIRES freertos spi_flash esp_partition esp_psram esp_hw_support esp_rom hal esp_mm esp_hal_wdt
                       INCLUDE_DIRS ".")
