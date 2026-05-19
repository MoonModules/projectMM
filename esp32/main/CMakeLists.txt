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
