# STATIC, not INTERFACE, now that JamWindowContent needs a .cpp - same
# reasoning as modules/jamn_core/CMakeLists.txt.
#
# No jamn_dsp dependency, deliberately: JamWindowContent reports what the
# user did through two std::functions and knows nothing about what happens
# next (see jam_window_content.h).
add_library(jamn_ui STATIC
    src/jam_window_content.cpp
    src/note_keyboard.cpp
)
target_include_directories(jamn_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(jamn_ui PUBLIC juce::juce_gui_basics)
target_compile_features(jamn_ui PUBLIC cxx_std_20)

if(BUILD_TESTING)
    # The first test target this module has had, and it constructs no
    # Component: PitchAtPoint takes ints so the hit testing can be checked
    # without a display. "app", not "fast" - this module links JUCE, and
    # the fast list must be identical under core-only and the full build
    # (AGENTS.md, Fast path).
    add_executable(jamn_ui_tests
        tests/note_keyboard_tests.cpp
    )
    target_link_libraries(jamn_ui_tests PRIVATE
        jamn_ui
        Catch2::Catch2WithMain
    )
    catch_discover_tests(jamn_ui_tests PROPERTIES LABELS "app")
endif()
