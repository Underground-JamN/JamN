# STATIC, not INTERFACE, now that JamWindowContent needs a .cpp - same
# reasoning as modules/jamn_core/CMakeLists.txt.
#
# No jamn_dsp dependency, deliberately: JamWindowContent reports what the
# user did through two std::functions and knows nothing about what happens
# next (see jam_window_content.h).
add_library(jamn_ui STATIC
    src/jam_window_content.cpp
)
target_include_directories(jamn_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(jamn_ui PUBLIC juce::juce_gui_basics)
target_compile_features(jamn_ui PUBLIC cxx_std_20)
