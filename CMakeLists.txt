cmake_minimum_required(VERSION 3.20)
project(projectMM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warning flags differ between GCC/Clang and MSVC. Gate by compiler so the
# desktop build runs cleanly on Windows runners (no -W flags on MSVC) and on
# macOS/Linux (no /W flags on Clang/GCC).
if(MSVC)
    add_compile_options(/W4 /WX)
    # Static MSVC runtime so the Windows binary doesn't need vcredist.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# Core library. Most modules ship header-only (single .h file with implementation
# inline). Core service modules that bridge to the platform (HTTP server,
# filesystem, network, …) ship as .h + .cpp per CLAUDE.md — list those .cpps
# here so both the application and the test binary link them.
add_library(mm_core STATIC
    src/core/Control.cpp
    src/core/HttpServerModule.cpp
    src/core/FilesystemModule.cpp
    src/core/Scheduler.cpp
)
target_include_directories(mm_core PUBLIC src/)
target_link_libraries(mm_core PUBLIC mm_platform)
# `add_dependencies(mm_core ui_embed)` is below, after the ui_embed target is defined.

# Platform library (desktop)
add_library(mm_platform src/platform/desktop/platform_desktop.cpp)
target_include_directories(mm_platform PUBLIC src/ src/platform/desktop/)

# Generate build_info.h from library.json (carries version, build date, board name).
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/core/build_info.h
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_SOURCE_DIR}/scripts/build/generate_build_info.py
    DEPENDS ${CMAKE_SOURCE_DIR}/library.json ${CMAKE_SOURCE_DIR}/scripts/build/generate_build_info.py
    COMMENT "Generating build_info.h"
)
add_custom_target(build_info_gen DEPENDS ${CMAKE_SOURCE_DIR}/src/core/build_info.h)

# Embed UI files as C arrays
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_SOURCE_DIR}/src/ui -DOUT=${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h -P ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/index.html ${CMAKE_SOURCE_DIR}/src/ui/app.js ${CMAKE_SOURCE_DIR}/src/ui/style.css ${CMAKE_SOURCE_DIR}/src/ui/release-picker.js
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h)

# mm_core's HttpServerModule.cpp consumes ui_embedded.h — wire the dep so a
# clean build orders them correctly.
add_dependencies(mm_core ui_embed)

# Application
add_executable(projectMM src/main.cpp src/platform/desktop/main_desktop.cpp)
target_link_libraries(projectMM PRIVATE mm_core mm_platform)
add_dependencies(projectMM ui_embed build_info_gen)

# Tests
enable_testing()
add_subdirectory(test)
