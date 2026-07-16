idf_component_register(SRCS main.c display.c
                       REQUIRES freertos spi_flash esp_partition esp_psram esp_hw_support esp_rom hal esp_mm esp_hal_wdt esp_lcd esp_hal_lcd esp_hal_dma heap
                       INCLUDE_DIRS ".")
