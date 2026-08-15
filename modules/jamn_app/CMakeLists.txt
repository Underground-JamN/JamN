add_executable(jamn_app src/main.cpp)
target_link_libraries(jamn_app PRIVATE
    jamn_core
    jamn_dsp
    jamn_engine
    jamn_session
    jamn_platform
    jamn_ui
    # The bench JSON schema, so --bench-json emits the same rows jamn_bench
    # does rather than a second format. jamn_bench_lib is JUCE-free and
    # stays that way; this edge runs the other direction, and tools/ is
    # added before this directory in the root CMakeLists so the target
    # already exists here.
    jamn_bench_lib
    juce::juce_core
    juce::juce_gui_basics
)
# The version the app reports comes from the one in CMakeLists.txt rather
# than a second copy in the source. It was "0.0.1" here until 2026-08-15,
# which was both the wrong number and the wrong scheme - CHANGELOG.md's
# versioning is MAJOR.MINOR, no patch component.
target_compile_definitions(jamn_app PRIVATE JAMN_VERSION_STRING="${PROJECT_VERSION}")
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

    # T5.0's accept, mechanically: two shipping binaries exchange notes over
    # a real socket and both shut down clean.
    #
    # Labelled `net`, not `app`, even though this binary links JUCE. Both
    # labels run only in the full build, so either would execute - but `net`
    # is what the gate scripts use to mean "binds a real socket", and that
    # is the property that decides whether this case can run at all. Adding
    # both labels would just run it twice.
    #
    # Not a Catch2 binary, same as the T4.3 checkpoint it extends: the
    # assertions live in the two processes' exit codes, because what is
    # under test is two processes that share no memory agreeing.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_test(
        NAME "Two jamn_app processes exchange notes and join their net threads cleanly"
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_two_process_app.py
                $<TARGET_FILE:jamn_app>
    )
    # Outermost bound on a hang, after jamn_app's own wall-clock stop and
    # the driver's subprocess timeout.
    set_tests_properties("Two jamn_app processes exchange notes and join their net threads cleanly"
        PROPERTIES LABELS "net" TIMEOUT 120
    )
endif()
