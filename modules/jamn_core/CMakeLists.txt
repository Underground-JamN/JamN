# STATIC, not INTERFACE, now that FileAudioDevice needs a .cpp - SpscRing
# and RealtimeScope stay header-only (templates/inline), so they cost
# nothing extra by living in a compiled library.
add_library(jamn_core STATIC
    src/file_audio_device.cpp
)
target_include_directories(jamn_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(jamn_core PUBLIC cxx_std_20)

if(BUILD_TESTING)
    # OBJECT, not STATIC: this TU defines global operator new/delete
    # replacements that nothing references by name. A static library's
    # member would never be pulled in and the allocation trap would
    # silently do nothing - the same class of bug as the TSan allocator
    # elision in CHANGELOG.md. An object library always contributes its
    # objects to whatever links it, so jamn_dsp_tests can share this trap
    # instead of duplicating it.
    add_library(jamn_core_rt_alloc_guard OBJECT tests/realtime_allocation_guard.cpp)
    target_link_libraries(jamn_core_rt_alloc_guard PUBLIC jamn_core)

    add_executable(jamn_core_tests
        tests/spsc_ring_tests.cpp
        tests/realtime_scope_tests.cpp
        tests/file_audio_device_tests.cpp
        tests/time_types_tests.cpp
        tests/clock_tests.cpp
        tests/byte_reader_writer_tests.cpp
        tests/histogram64_tests.cpp
    )
    target_link_libraries(jamn_core_tests PRIVATE jamn_core jamn_core_rt_alloc_guard Catch2::Catch2WithMain)
    catch_discover_tests(jamn_core_tests PROPERTIES LABELS "fast")
endif()
