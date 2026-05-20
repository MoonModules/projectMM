cmake_minimum_required(VERSION 3.20)
project(mmv3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

# Core library (header-only: all MoonModules are single .h files)
add_library(mm_core INTERFACE)
target_include_directories(mm_core INTERFACE src/)

# Platform library (desktop)
add_library(mm_platform src/platform/desktop/platform_desktop.cpp)
target_include_directories(mm_platform PUBLIC src/ src/platform/desktop/)

# Embed UI files as C arrays
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_SOURCE_DIR}/src/ui -DOUT=${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h -P ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/index.html ${CMAKE_SOURCE_DIR}/src/ui/app.js ${CMAKE_SOURCE_DIR}/src/ui/style.css
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h)

# Application
add_executable(mmv3 src/main.cpp src/platform/desktop/main_desktop.cpp)
target_link_libraries(mmv3 PRIVATE mm_core mm_platform)
add_dependencies(mmv3 ui_embed)

# Tests
enable_testing()
add_subdirectory(test)
