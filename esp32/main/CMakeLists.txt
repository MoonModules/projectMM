idf_component_register(
    SRCS
        "main.cpp"
        "../../src/main.cpp"
        "../../src/core/Control.cpp"
        "../../src/core/HttpServerModule.cpp"
        "../../src/core/FilesystemModule.cpp"
        "../../src/core/Scheduler.cpp"
        "../../src/platform/esp32/platform_esp32.cpp"
        "../../src/platform/esp32/platform_esp32_fs.cpp"
        "../../src/platform/esp32/platform_esp32_ota.cpp"
        "../../src/platform/esp32/platform_esp32_improv.cpp"
    INCLUDE_DIRS
        "../../src"
        "../../src/platform/esp32"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Firmware-variant defines, set by scripts/build/build_esp32.py via firmware_cmake_args().
# See docs/architecture.md § Firmware vs board — "firmware" is the compiled
# binary variant; the physical "board" is a separate concept the device
# cannot identify on its own.
#
# MM_FIRMWARE_NAME — the firmware variant key (`esp32`, `esp32-eth`,
#                 `esp32-eth-wifi`, `esp32s3-n16r8`). Surfaced by
#                 SystemModule.firmware on the device card and used by
#                 FirmwareUpdateModule's release picker to filter to
#                 OTA-compatible binaries. Cache variables in ESP-IDF reach
#                 this CMakeLists but not the component compile line by
#                 default, so we forward it explicitly. Without this,
#                 build_info.h's `#ifndef` fallback kicks in and the device
#                 reports "firmware: unknown".
#
# MM_ETH_ONLY  — set on `--firmware esp32-eth` (no WiFi compiled in).
#                The script also passes EXCLUDE_COMPONENTS for the WiFi libs;
#                this define gates the WiFi source paths in platform_esp32.cpp
#                and the platform::hasWiFi compile-time flag.
#
# MM_NO_ETH    — set on firmwares that don't enable CONFIG_ETH_USE_ESP32_EMAC
#                (today: `--firmware esp32`, `--firmware esp32s3-n16r8`).
#                Without the EMAC headers, the on-chip Ethernet code in
#                platform_esp32.cpp won't compile; this define stubs the
#                eth* platform functions to return false/empty, mirroring
#                the desktop platform layer.
if(MM_FIRMWARE_NAME)
    # build_esp32.py passes -DMM_FIRMWARE_NAME="<key>" where the outer quotes
    # become part of the CMake variable value (verified via CMakeCache.txt).
    # So ${MM_FIRMWARE_NAME} already expands to "esp32-eth" *with* the quotes —
    # don't re-add them here, or the preprocessor sees MM_FIRMWARE_NAME=""esp32""
    # and fails as if "esp32" were an identifier.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_FIRMWARE_NAME=${MM_FIRMWARE_NAME})
endif()
if(MM_RELEASE)
    # Same quoting note as MM_FIRMWARE_NAME above — the value arrives with its
    # quotes already baked in. Release-channel tag (latest / vX.Y.Z); only the
    # release workflow sets it, so local builds fall through to build_info.h's
    # #ifndef "" default.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_RELEASE=${MM_RELEASE})
endif()
if(MM_ETH_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_NO_WIFI)
endif()
if(MM_NO_ETH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_NO_ETH)
endif()

# Embed UI files — regenerate on every build
set(UI_DIR ${COMPONENT_DIR}/../../src/ui)
add_custom_command(
    OUTPUT ${UI_DIR}/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DOUT=${UI_DIR}/ui_embedded.h -P ${UI_DIR}/embed_ui.cmake
    DEPENDS ${UI_DIR}/index.html ${UI_DIR}/app.js ${UI_DIR}/style.css ${UI_DIR}/install-picker.js
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${UI_DIR}/ui_embedded.h)

# Generate build_info.h from library.json (carries version, build date, board name).
set(BUILD_INFO_DIR ${COMPONENT_DIR}/../../src/core)
add_custom_command(
    OUTPUT ${BUILD_INFO_DIR}/build_info.h
    COMMAND python3 ${COMPONENT_DIR}/../../scripts/build/generate_build_info.py
    DEPENDS ${COMPONENT_DIR}/../../library.json ${COMPONENT_DIR}/../../scripts/build/generate_build_info.py
    COMMENT "Generating build_info.h"
)
add_custom_target(build_info_gen DEPENDS ${BUILD_INFO_DIR}/build_info.h)

add_dependencies(${COMPONENT_LIB} ui_embed build_info_gen)
