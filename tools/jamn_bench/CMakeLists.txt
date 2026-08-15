add_library(jamn_bench_lib STATIC
    src/bench_result.cpp
)
target_include_directories(jamn_bench_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# PUBLIC because bench_result.h itself includes jamn_core/audio_block_timing.h
# - the reading a real device produces. jamn_core is JUCE-free, so this does
# not cost this library the property that lets jamn_app link it without
# dragging JUCE into the core-only preset.
target_link_libraries(jamn_bench_lib PUBLIC jamn_core)
target_compile_features(jamn_bench_lib PUBLIC cxx_std_20)

add_executable(jamn_bench src/main.cpp)
target_link_libraries(jamn_bench PRIVATE jamn_bench_lib jamn_core jamn_engine)
target_compile_features(jamn_bench PRIVATE cxx_std_20)

# The loopback-clock backend needs EnetTransport, which only exists in a
# full build. The guard is JAMN_CORE_ONLY and not "is the enet target
# defined yet" for the reason modules/jamn_net/CMakeLists.txt spells out:
# this directory is added before the root's ENet fetch, so an
# immediate-evaluation test would be false in both presets and would
# silently produce a full build with the backend compiled out.
if(NOT JAMN_CORE_ONLY)
    target_compile_definitions(jamn_bench PRIVATE JAMN_BENCH_HAS_ENET=1)

    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        # A short smoke run under the `net` label, so the backend cannot rot
        # unnoticed between manual acceptance runs. Six seconds is enough to
        # clear ClockSync's 8-sample lock at its 4Hz fast cadence and take a
        # few readings - it is a liveness check, not criterion #1's reading,
        # which is 30 wall-clock minutes and stays a manual step.
        add_test(
            NAME "The loopback-clock bench backend locks and reports a plausible offset p99"
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_loopback_clock.py
                    $<TARGET_FILE:jamn_bench> 6
        )
        set_tests_properties("The loopback-clock bench backend locks and reports a plausible offset p99"
            PROPERTIES LABELS "net" TIMEOUT 120
        )
    endif()
endif()

if(BUILD_TESTING)
    add_executable(jamn_bench_tests
        tests/bench_result_tests.cpp
    )
    target_link_libraries(jamn_bench_tests PRIVATE jamn_bench_lib Catch2::Catch2WithMain)
    catch_discover_tests(jamn_bench_tests PROPERTIES LABELS "fast")
endif()
