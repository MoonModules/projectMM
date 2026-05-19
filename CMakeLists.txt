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
target_include_directories(mm_platform PUBLIC src/)

# Application
add_executable(mmv3 src/main.cpp)
target_link_libraries(mmv3 PRIVATE mm_core mm_platform)

# Tests
enable_testing()
add_subdirectory(test)
