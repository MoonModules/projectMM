idf_component_register(
    SRCS
        "main.cpp"
        "../../src/main.cpp"
        "../../src/core/HttpServerModule.cpp"
        "../../src/core/FilesystemModule.cpp"
        "../../src/core/Scheduler.cpp"
        "../../src/platform/esp32/platform_esp32.cpp"
    INCLUDE_DIRS
        "../../src"
        "../../src/platform/esp32"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Build-variant defines, set by scripts/build/build_esp32.py via board_cmake_args().
#
# MM_ETH_ONLY  — set on `--board esp32-eth` (no WiFi compiled in).
#                The script also passes EXCLUDE_COMPONENTS for the WiFi libs;
#                this define gates the WiFi source paths in platform_esp32.cpp
#                and the platform::hasWiFi compile-time flag.
#
# MM_NO_ETH    — set on boards that don't enable CONFIG_ETH_USE_ESP32_EMAC
#                (today: `--board esp32`, `--board esp32s3-n16r8`). Without
#                the EMAC headers, the on-chip Ethernet code in
#                platform_esp32.cpp won't compile; this define stubs the
#                eth* platform functions to return false/empty, mirroring
#                the desktop platform layer.
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
    DEPENDS ${UI_DIR}/index.html ${UI_DIR}/app.js ${UI_DIR}/style.css
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
