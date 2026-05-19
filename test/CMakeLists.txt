add_executable(mm_tests
    test_color.cpp
    test_buffer.cpp
    test_moonmodule.cpp
    test_grid_layout.cpp
    test_rainbow.cpp
    test_artnet_packet.cpp
)

target_include_directories(mm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_tests PRIVATE mm_core mm_platform)

add_test(NAME unit_tests COMMAND mm_tests)

# Scenario runner (replays scenario JSON files in-process)
add_executable(mm_scenarios scenario_runner.cpp)
target_link_libraries(mm_scenarios PRIVATE mm_core mm_platform)
