add_library(mm_light STATIC
    MappingLUT.cpp
)
target_include_directories(mm_light PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mm_light PUBLIC mm_core mm_platform)
