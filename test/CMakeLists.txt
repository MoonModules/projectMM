# Unit tests, split between core/ and light/ to mirror src/.
# A test stays in the subfolder whose domain its primary @module belongs to;
# cross-domain references travel through the @also list, not the directory.
add_executable(mm_tests
    # core/
    unit/core/unit_Buffer.cpp
    unit/core/unit_Color.cpp
    unit/core/unit_math8.cpp
    unit/core/unit_noise.cpp
    unit/core/unit_crc.cpp
    unit/core/unit_Control_apply_absent_key.cpp
    unit/core/unit_Control_list.cpp
    unit/core/unit_DeviceIdentify.cpp
    unit/core/unit_DevicesModule_ageout.cpp
    unit/core/unit_DevicesModule_discovery.cpp
    unit/core/unit_DevicesModule_hue.cpp
    unit/core/unit_WledPacket.cpp
    unit/core/unit_FilesystemModule_persistence.cpp
    unit/core/unit_FirmwareUpdateModule.cpp
    unit/core/unit_HttpServerModule_apply.cpp
    unit/core/unit_ImprovFrame.cpp
    unit/core/unit_ImprovOpReassembler.cpp
    unit/core/unit_JsonUtil_parse.cpp
    unit/core/unit_MappingLUT.cpp
    unit/core/unit_ModuleFactory.cpp
    unit/core/unit_moonlive_fill.cpp
    unit/core/unit_moonlive_compiler.cpp
    unit/core/unit_moonlive_ir.cpp
    unit/core/unit_MoonModule.cpp
    unit/core/unit_MoonModule_control_change_gate.cpp
    unit/core/unit_MoonModule_lifecycle.cpp
    unit/core/unit_MoonModule_movechild.cpp
    unit/core/unit_MoonModule_replacechild.cpp
    unit/core/unit_NetworkModule.cpp
    unit/core/unit_NetworkModule_ethernet.cpp
    unit/core/unit_Sort.cpp
    unit/core/unit_sanitizeHostname.cpp
    unit/core/unit_platform_clock.cpp
    unit/core/unit_Scheduler_unique_names.cpp
    unit/core/unit_SystemModule.cpp

    # light/
    unit/light/unit_AudioModule.cpp
    unit/light/unit_AudioLevel.cpp
    unit/light/unit_AudioBands.cpp
    unit/light/unit_NetworkSendDriver_no_alloc_in_loop.cpp
    unit/light/unit_NetworkSendDriver_packet.cpp
    unit/light/unit_BlendMap.cpp
    unit/light/unit_draw.cpp
    unit/light/unit_GameOfLifeEffect.cpp
    unit/light/unit_DemoReelEffect.cpp
    unit/light/unit_TextEffect.cpp
    unit/light/unit_Coord3D.cpp
    unit/light/unit_effects_render.cpp
    unit/light/unit_SineEffect.cpp
    unit/light/unit_DistortionWavesEffect.cpp
    unit/light/unit_Correction.cpp
    unit/light/unit_Drivers_container.cpp
    unit/light/unit_Drivers_firstOutputRgb.cpp
    unit/light/unit_HueDriver.cpp
    unit/light/unit_Palette.cpp
    unit/light/unit_WaveEffect.cpp
    unit/light/unit_FireEffect.cpp
    unit/light/unit_GridLayout.cpp
    unit/light/unit_SphereLayout.cpp
    unit/light/unit_WheelLayout.cpp
    unit/light/unit_Layer_extrude.cpp
    unit/light/unit_Layer_persistence.cpp
    unit/light/unit_Layer_sparse_mapping.cpp
    unit/light/unit_Layer_modifier_chain.cpp
    unit/light/unit_Layer_live_modifier.cpp
    unit/light/unit_Layer_phase_animation.cpp
    unit/light/unit_Layer_zero_grid.cpp
    unit/light/unit_Layers_container.cpp
    unit/light/unit_Layouts_container.cpp
    unit/light/unit_Layouts_mutation.cpp
    unit/light/unit_Layouts_toggle_cycle.cpp
    unit/light/unit_MetaballsEffect.cpp
    unit/light/unit_MultiplyModifier.cpp
    unit/light/unit_CheckerboardModifier.cpp
    unit/light/unit_RandomMapModifier.cpp
    unit/light/unit_RotateModifier.cpp
    unit/light/unit_RegionModifier.cpp
    unit/light/unit_PinwheelModifier.cpp
    unit/light/unit_NoiseEffect.cpp
    unit/light/unit_ParticlesEffect.cpp
    unit/light/unit_PlasmaEffect.cpp
    unit/light/unit_PreviewDriver.cpp
    unit/light/unit_RainbowEffect.cpp
    unit/light/unit_NetworkReceiveEffect.cpp
    unit/light/unit_NetworkReceiveEffect_protocols.cpp
    unit/light/unit_LcdLedDriver.cpp
    unit/light/unit_LcdLedEncoder.cpp
    unit/light/unit_ParlioLedDriver.cpp
    unit/light/unit_RmtLedEncoder.cpp
    unit/light/unit_RmtLedDriver_lifecycle.cpp
    unit/light/unit_RmtLedDriver_pins.cpp
)

target_include_directories(mm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_tests PRIVATE mm_core mm_platform)

add_test(NAME unit_tests COMMAND mm_tests)

# Scenario runner (replays scenario JSON files in-process)
add_executable(mm_scenarios scenario_runner.cpp)
target_link_libraries(mm_scenarios PRIVATE mm_core mm_platform)
