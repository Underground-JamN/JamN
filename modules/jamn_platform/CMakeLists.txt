# STATIC, not INTERFACE, now that JuceAudioDevice needs a .cpp - same
# reasoning as modules/jamn_core/CMakeLists.txt.
add_library(jamn_platform STATIC
    src/juce_audio_device.cpp
    src/key_note_input.cpp
)
target_include_directories(jamn_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# New module edge, jamn_platform -> jamn_core: JuceAudioDevice bridges JUCE
# into jamn::core::AudioCallback and wraps it in jamn::core::RealtimeScope.
target_link_libraries(jamn_platform PUBLIC jamn_core juce::juce_audio_devices)
target_compile_features(jamn_platform PUBLIC cxx_std_20)

if(BUILD_TESTING)
    # The first test target this module has had. KeyNoteInput itself
    # includes no JUCE, but this binary links jamn_platform and therefore
    # JUCE - so it carries "app", not "fast". `ctest -L fast` is the
    # JUCE-free suite and its list must be identical under core-only and
    # the full build (AGENTS.md, Fast path), and this module is not built
    # under core-only at all.
    add_executable(jamn_platform_tests
        tests/key_note_input_tests.cpp
    )
    target_link_libraries(jamn_platform_tests PRIVATE
        jamn_platform
        Catch2::Catch2WithMain
    )
    catch_discover_tests(jamn_platform_tests PROPERTIES LABELS "app")
endif()
