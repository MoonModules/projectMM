idf_component_register(
    SRCS
        "main.cpp"
        "../../src/main.cpp"
        "../../src/platform/esp32/platform_esp32.cpp"
    INCLUDE_DIRS
        "../../src"
        "../../src/platform/esp32"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Ethernet-only build profile: build_esp32.py --profile eth-only passes
# -DMM_ETH_ONLY=1 (and EXCLUDE_COMPONENTS for the WiFi components). MM_NO_WIFI
# makes platform_config.h report hasWiFi=false and #ifdefs-out the WiFi code
# in platform_esp32.cpp.
if(MM_ETH_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_NO_WIFI)
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

# Generate version.h from library.json
set(VERSION_DIR ${COMPONENT_DIR}/../../src/core)
add_custom_command(
    OUTPUT ${VERSION_DIR}/version.h
    COMMAND python3 ${COMPONENT_DIR}/../../scripts/build/generate_version.py
    DEPENDS ${COMPONENT_DIR}/../../library.json ${COMPONENT_DIR}/../../scripts/build/generate_version.py
    COMMENT "Generating version.h"
)
add_custom_target(version_gen DEPENDS ${VERSION_DIR}/version.h)

add_dependencies(${COMPONENT_LIB} ui_embed version_gen)
