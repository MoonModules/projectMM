cmake_minimum_required(VERSION 3.20)
project(projectMM-v3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/light)
add_subdirectory(src/app)

enable_testing()
add_subdirectory(test)
