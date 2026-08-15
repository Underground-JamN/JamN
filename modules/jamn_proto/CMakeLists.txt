# STATIC, not INTERFACE, now that PacketHeader/negotiation need .cpp files -
# same reasoning as modules/jamn_dsp/CMakeLists.txt.
add_library(jamn_proto STATIC
    src/packet_header.cpp
    src/negotiation.cpp
    src/note_event.cpp
    src/note_burst.cpp
    src/hello.cpp
    src/session_config.cpp
    src/instrument_assign.cpp
    src/join_auth.cpp
    src/clock_pingpong.cpp
    src/leave.cpp
)
target_include_directories(jamn_proto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# jamn_proto -> jamn_core: ByteReader/ByteWriter for wire encode/decode.
target_link_libraries(jamn_proto PUBLIC jamn_core)
target_compile_features(jamn_proto PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_executable(jamn_proto_tests
        tests/packet_header_tests.cpp
        tests/tlv_tests.cpp
        tests/negotiation_tests.cpp
        tests/note_event_tests.cpp
        tests/note_burst_tests.cpp
        tests/hello_tests.cpp
        tests/session_config_tests.cpp
        tests/instrument_assign_tests.cpp
        tests/proto_golden_tests.cpp
        tests/proto_fuzz_replay_tests.cpp
        tests/join_auth_tests.cpp
        tests/clock_pingpong_tests.cpp
        tests/leave_tests.cpp
    )
    target_link_libraries(jamn_proto_tests PRIVATE jamn_proto Catch2::Catch2WithMain)
    # Absolute paths, not relative - ctest's working directory is the build
    # tree, not this source directory.
    target_compile_definitions(jamn_proto_tests PRIVATE
        JAMN_PROTO_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        JAMN_PROTO_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_corpus"
    )
    catch_discover_tests(jamn_proto_tests PROPERTIES LABELS "fast")
endif()
