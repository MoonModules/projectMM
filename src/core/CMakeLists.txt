add_library(mm_core STATIC
    MoonModule.cpp
)
target_include_directories(mm_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
