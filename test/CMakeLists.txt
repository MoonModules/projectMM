add_executable(mm_tests
    test_color.cpp
    test_buffer.cpp
    test_moonmodule.cpp
    test_system_module.cpp
    test_grid_layout.cpp
    test_rainbow.cpp
    test_extrude.cpp
    test_artnet_packet.cpp
    test_noise.cpp
    test_mapping_lut.cpp
    test_mirror.cpp
    test_blend_map.cpp
    test_filesystem_persistence.cpp
    test_movechild.cpp
    test_replacechild.cpp
    test_module_factory.cpp
    test_preview_driver.cpp
    test_fire.cpp
    test_metaballs.cpp
    test_particles.cpp
    test_plasma.cpp
    test_stateless_effects.cpp
)

target_include_directories(mm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_tests PRIVATE mm_core mm_platform)

add_test(NAME unit_tests COMMAND mm_tests)

# Scenario runner (replays scenario JSON files in-process)
add_executable(mm_scenarios scenario_runner.cpp)
target_link_libraries(mm_scenarios PRIVATE mm_core mm_platform)
