if(CONFIG_S31_LINUX_BOARD_KORVO1)
    set(board_srcs board_korvo1.c display.c rmt.c sd.c)
else()
    set(board_srcs board_esp32s31.c)
endif()

idf_component_register(SRCS main.c ${board_srcs}
                       REQUIRES freertos spi_flash esp_partition esp_psram esp_hw_support esp_rom hal esp_mm esp_hal_wdt esp_lcd esp_hal_lcd esp_hal_dma esp_hal_sd esp_hal_rmt esp_driver_gpio heap esp_hosted bootloader_support esp_coex
                       INCLUDE_DIRS ".")
