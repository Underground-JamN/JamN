# STATIC, not INTERFACE, now that JuceAudioDevice needs a .cpp - same
# reasoning as modules/jamn_core/CMakeLists.txt.
add_library(jamn_platform STATIC
    src/juce_audio_device.cpp
)
target_include_directories(jamn_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# New module edge, jamn_platform -> jamn_core: JuceAudioDevice bridges JUCE
# into jamn::core::AudioCallback and wraps it in jamn::core::RealtimeScope.
target_link_libraries(jamn_platform PUBLIC jamn_core juce::juce_audio_devices)
target_compile_features(jamn_platform PUBLIC cxx_std_20)
