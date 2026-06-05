# Unit tests, split between core/ and light/ to mirror src/.
# A test stays in the subfolder whose domain its primary @module belongs to;
# cross-domain references travel through the @also list, not the directory.
add_executable(mm_tests
    # core/
    unit/core/unit_BoardModule.cpp
    unit/core/unit_Buffer.cpp
    unit/core/unit_Color.cpp
    unit/core/unit_FilesystemModule_persistence.cpp
    unit/core/unit_ImprovFrame.cpp
    unit/core/unit_MappingLUT.cpp
    unit/core/unit_ModuleFactory.cpp
    unit/core/unit_MoonModule.cpp
    unit/core/unit_MoonModule_control_change_gate.cpp
    unit/core/unit_MoonModule_lifecycle.cpp
    unit/core/unit_MoonModule_movechild.cpp
    unit/core/unit_MoonModule_replacechild.cpp
    unit/core/unit_NetworkModule.cpp
    unit/core/unit_platform_clock.cpp
    unit/core/unit_Scheduler_unique_names.cpp
    unit/core/unit_SystemModule.cpp

    # light/
    unit/light/unit_ArtNetSendDriver_no_alloc_in_loop.cpp
    unit/light/unit_ArtNetSendDriver_packet.cpp
    unit/light/unit_BlendMap.cpp
    unit/light/unit_CheckerboardEffect.cpp
    unit/light/unit_Correction.cpp
    unit/light/unit_Drivers_container.cpp
    unit/light/unit_FireEffect.cpp
    unit/light/unit_GridLayout.cpp
    unit/light/unit_SphereLayout.cpp
    unit/light/unit_Layer_extrude.cpp
    unit/light/unit_Layer_sparse_mapping.cpp
    unit/light/unit_Layer_phase_animation.cpp
    unit/light/unit_Layer_zero_grid.cpp
    unit/light/unit_Layers_container.cpp
    unit/light/unit_Layouts_container.cpp
    unit/light/unit_Layouts_mutation.cpp
    unit/light/unit_Layouts_toggle_cycle.cpp
    unit/light/unit_MetaballsEffect.cpp
    unit/light/unit_MirrorModifier.cpp
    unit/light/unit_NoiseEffect.cpp
    unit/light/unit_ParticlesEffect.cpp
    unit/light/unit_PlasmaEffect.cpp
    unit/light/unit_PreviewDriver.cpp
    unit/light/unit_RainbowEffect.cpp
)

target_include_directories(mm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_tests PRIVATE mm_core mm_platform)

add_test(NAME unit_tests COMMAND mm_tests)

# Scenario runner (replays scenario JSON files in-process)
add_executable(mm_scenarios scenario_runner.cpp)
target_link_libraries(mm_scenarios PRIVATE mm_core mm_platform)
