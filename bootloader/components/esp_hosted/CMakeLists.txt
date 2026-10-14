# The ESP-Hosted firmware (external/esp-hosted-linux) as a component of the
# loader, with the shared-memory transport in place of the USB transport.
# These are the sources, archives and link options of its network_adapter
# component.  Its app_main() becomes esp_hosted_main(), which the loader calls.
set(hosted ${CMAKE_CURRENT_LIST_DIR}/../../../external/esp-hosted-linux/esp/esp_driver)
set(hosted_main ${hosted}/network_adapter/main)

idf_component_register(
    SRCS ${hosted_main}/app_main.c ${hosted_main}/slave_bt.c
         ${hosted_main}/cmd.c ${hosted_main}/stats.c shm_slave_api.c
    INCLUDE_DIRS ${hosted_main}/include ../../../shared
    REQUIRES esp_wifi esp_event nvs_flash app_update protocomm
             wpa_supplicant esp_timer esp_driver_gpio esp_driver_uart
             esp_hw_support esp_system heap freertos log)

set_source_files_properties(${hosted_main}/app_main.c
    PROPERTIES COMPILE_DEFINITIONS "app_main=esp_hosted_main")

idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")

# The firmware calls private 802.11 functions of its own Wi-Fi archives.
idf_build_get_property(target IDF_TARGET)
foreach(blob core espnow mesh net80211 pp smartconfig wapi target)
    if(TARGET esp_wifi_${blob})
        set(archive "${hosted}/lib/${target}/lib${blob}.a")
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "ESP-Hosted Wi-Fi archive missing: ${archive}")
        endif()
        set_property(TARGET esp_wifi_${blob} PROPERTY IMPORTED_LOCATION "${archive}")
    endif()
endforeach()

# The supplicant runs on Linux.  Only the IE parser of esp_wpa_main.o stays.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_supplicant_init"
    "-Wl,--wrap=esp_supplicant_deinit")
