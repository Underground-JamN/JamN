# STATIC, not INTERFACE, now that ClockSync needs a .cpp - same reasoning
# as modules/jamn_dsp/CMakeLists.txt.
add_library(jamn_engine STATIC
    src/audio_clock.cpp
    src/audio_runtime.cpp
    src/clock_sync.cpp
    src/jitter_buffer.cpp
    src/event_scheduler.cpp
    src/dedupe_ring.cpp
    src/burst_assembler.cpp
    src/note_crossing.cpp
    src/peer_runtime.cpp
    src/net_thread.cpp
)
target_include_directories(jamn_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# jamn_engine -> jamn_core: Histogram64 for JitterBuffer.
# jamn_engine -> jamn_proto: NoteEvent, scheduled by EventScheduler.
# jamn_engine -> jamn_net: PeerId, the identity EventScheduler schedules against.
# Threads::Threads: NetThread owns the net thread itself. Not a new module
# edge - this module is the first one in the project to create a thread, so
# it is the first one that has to say so to CMake.
find_package(Threads REQUIRED)
target_link_libraries(jamn_engine PUBLIC jamn_core jamn_proto jamn_net Threads::Threads)
target_compile_features(jamn_engine PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_executable(jamn_engine_tests
        tests/audio_clock_tests.cpp
        tests/audio_runtime_tests.cpp
        tests/clock_sync_tests.cpp
        tests/jitter_buffer_tests.cpp
        tests/event_scheduler_tests.cpp
        tests/serial_compare_tests.cpp
        tests/dedupe_ring_tests.cpp
        tests/burst_assembler_tests.cpp
        tests/note_crossing_tests.cpp
        tests/peer_runtime_tests.cpp
        tests/net_thread_tests.cpp
        tests/six_node_acceptance_tests.cpp
        tests/clock_accuracy_sim_tests.cpp
    )
    target_link_libraries(jamn_engine_tests PRIVATE jamn_engine jamn_core_rt_alloc_guard Catch2::Catch2WithMain)
    catch_discover_tests(jamn_engine_tests PROPERTIES LABELS "fast")
endif()

# T4.3's checkpoint: two real processes over a real socket. Guarded on
# JAMN_CORE_ONLY because it needs EnetTransport, which that preset never
# fetches - and labelled `net` rather than `fast` for the same reason the
# ENet transport tests are (the fast list must be identical in both builds).
#
# Not a Catch2 binary: the assertions live in the peers' exit codes, because
# the thing under test is two processes agreeing, which no single in-process
# test framework can express.
if(BUILD_TESTING AND NOT JAMN_CORE_ONLY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(jamn_loopback_peer tests/loopback_peer_main.cpp)
    target_link_libraries(jamn_loopback_peer PRIVATE jamn_engine)

    add_test(
        NAME "Two processes over real sockets exchange notes with zero stuck notes"
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_two_process_loopback.py
                $<TARGET_FILE:jamn_loopback_peer>
    )
    # Third and outermost bound on a hang, after the peers' own wall-clock
    # stop and the driver's subprocess timeout. With no CI, a ctest that
    # hangs is worse than one that fails.
    set_tests_properties("Two processes over real sockets exchange notes with zero stuck notes"
        PROPERTIES LABELS "net" TIMEOUT 120
    )
endif()
