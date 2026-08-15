# STATIC, not INTERFACE, now that MasterBus/BlipVoice/JamAudio need .cpp
# files - same reasoning as modules/jamn_core/CMakeLists.txt.
add_library(jamn_dsp STATIC
    src/master_bus.cpp
    src/blip_voice.cpp
    src/jam_audio.cpp
    src/test_tone_instrument.cpp
    src/strip.cpp
    src/peer_mixer.cpp
)
target_include_directories(jamn_dsp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# New module edge, jamn_dsp -> jamn_core: SpscRing for the trigger queue (the
# one lock-free primitive docs/RT_RULES.md permits) and RealtimeScope in the
# tests below. jamn_core depends on nothing, so this adds no transitive
# weight.
target_link_libraries(jamn_dsp PUBLIC jamn_core)
target_compile_features(jamn_dsp PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_executable(jamn_dsp_tests
        tests/master_bus_tests.cpp
        tests/blip_voice_tests.cpp
        tests/jam_audio_tests.cpp
        tests/test_tone_instrument_tests.cpp
        tests/strip_tests.cpp
        tests/peer_mixer_tests.cpp
    )
    target_link_libraries(jamn_dsp_tests PRIVATE
        jamn_dsp
        jamn_core_rt_alloc_guard
        Catch2::Catch2WithMain
    )
    catch_discover_tests(jamn_dsp_tests PROPERTIES LABELS "fast")
endif()
