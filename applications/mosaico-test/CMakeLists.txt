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
set(MOSAICO_TEST_SOURCES platform.c tests.c ui.c instruments.c sensors.c audio.c)

function(configure_mosaico_target target)
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
    target_link_libraries(${target} PRIVATE lvgl ALSA::ALSA m)
endfunction()

add_executable(mosaico-test main.c ${MOSAICO_TEST_SOURCES})
configure_mosaico_target(mosaico-test)
install(TARGETS mosaico-test RUNTIME DESTINATION bin)

option(MOSAICO_TEST_BUILD_EXAMPLES "Build the host-only example renderer" OFF)
if(MOSAICO_TEST_BUILD_EXAMPLES)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Example rendering requires a native host build")
    endif()
    add_executable(mosaico-test-examples examples/render.c ${MOSAICO_TEST_SOURCES})
    target_include_directories(mosaico-test-examples PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    configure_mosaico_target(mosaico-test-examples)
endif()
