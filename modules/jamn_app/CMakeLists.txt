add_executable(jamn_app src/main.cpp)
target_link_libraries(jamn_app PRIVATE
    jamn_core
    jamn_dsp
    jamn_engine
    jamn_session
    jamn_platform
    jamn_ui
    juce::juce_core
    juce::juce_gui_basics
)
target_compile_features(jamn_app PRIVATE cxx_std_20)

if(BUILD_TESTING)
    # Runs the real jamn_app binary and asserts exit code 0 - catches
    # link-time-invisible failures (static-init crashes, missing JUCE
    # runtime config) the build alone won't. --headless makes it open the
    # audio device, pump a fixed number of blocks through FileAudioDevice
    # and exit, instead of opening a window and hanging ctest forever.
    #
    # Labelled "app", not "fast". `ctest -L fast` is the JUCE-free suite,
    # and its test list must be identical under core-only and the full
    # build (AGENTS.md, Fast path); this binary links JUCE, so it cannot be
    # in it. Both gate scripts run `-L app` as their own step - nothing
    # stopped running, it moved.
    add_test(NAME jamn_app_smoke COMMAND jamn_app --headless)
    set_tests_properties(jamn_app_smoke PROPERTIES LABELS "app" TIMEOUT 30)
endif()
