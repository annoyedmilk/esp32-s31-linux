# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(mosaico-test LANGUAGES C)

set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/lvgl" CACHE PATH
    "Pinned LVGL source submodule")
if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR "Initialize the external/lvgl submodule first")
endif()

# The board toolchain is C-only. None of the optional C++ renderers is used.
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_DIR}/src/*.c")
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC "${LVGL_DIR}")
target_compile_definitions(lvgl PUBLIC
    LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")
target_compile_options(lvgl PRIVATE -ffunction-sections -fdata-sections)

find_package(ALSA REQUIRED)
add_executable(mosaico-test main.c platform.c tests.c ui.c instruments.c sensors.c audio.c)
target_compile_features(mosaico-test PRIVATE c_std_11)
target_compile_definitions(mosaico-test PRIVATE _GNU_SOURCE)
target_compile_options(mosaico-test PRIVATE -Wall -Wextra -Werror
    -ffunction-sections -fdata-sections)
target_link_options(mosaico-test PRIVATE -Wl,--gc-sections)
target_link_libraries(mosaico-test PRIVATE lvgl ALSA::ALSA m)
install(TARGETS mosaico-test RUNTIME DESTINATION bin)
