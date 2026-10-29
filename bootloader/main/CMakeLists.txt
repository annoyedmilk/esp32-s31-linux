if(CONFIG_S31_LINUX_BOARD_KORVO1)
    set(board_srcs board_korvo1.c display.c)
elseif(CONFIG_S31_LINUX_BOARD_ESP_MOSAICO)
    set(board_srcs board_esp_mosaico.c)
else()
    set(board_srcs board_esp32s31.c)
endif()

idf_component_register(SRCS main.c radio_coex.c ${board_srcs}
                       REQUIRES freertos esp_partition esp_psram esp_hw_support esp_rom hal esp_hal_wdt esp_lcd esp_hal_lcd esp_hal_dma esp_driver_gpio heap esp_hosted bootloader_support openthread
                       INCLUDE_DIRS ".")

# radio_coex.c runs the Wi-Fi/802.15.4 coexistence only while the radio
# receives.  --undefined pulls its wrappers out of the archive.
foreach(f otPlatRadioReceive otPlatRadioSleep otPlatRadioDisable)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${f}" "-Wl,--undefined=__wrap_${f}")
endforeach()
