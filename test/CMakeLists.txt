add_executable(mm_tests
    test_main.cpp
    core/test_platform.cpp
    core/test_controls.cpp
    core/test_moonmodule.cpp
    core/test_scheduler.cpp
    core/test_http_server.cpp
    light/test_pixel.cpp
    light/test_buffer.cpp
    light/test_mapping.cpp
    light/test_noise.cpp
    light/test_grid_layout.cpp
    light/test_wheel_layout.cpp
    light/test_layout_group.cpp
    light/test_mirror_modifier.cpp
    light/test_rotate_modifier.cpp
    light/test_layer.cpp
    light/test_rainbow.cpp
    light/test_noise_effect.cpp
    light/test_blend_map.cpp
    light/test_driver_group.cpp
    light/test_artnet_send.cpp
)
target_include_directories(mm_tests PRIVATE ${CMAKE_SOURCE_DIR}/test)
target_link_libraries(mm_tests PRIVATE mm_core mm_platform mm_light)

include(CTest)
add_test(NAME mm_tests COMMAND mm_tests)
