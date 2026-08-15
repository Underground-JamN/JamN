# STATIC, not INTERFACE, now that Roster/SessionHost need .cpp files - same
# reasoning as modules/jamn_dsp/CMakeLists.txt.
add_library(jamn_session STATIC
    src/roster.cpp
    src/session_host.cpp
)
target_include_directories(jamn_session PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# jamn_session -> jamn_net: PeerId/PeerEvent/kMaxPeers, and the ITransport
# the host acts on.
# jamn_session -> jamn_proto: Hello, Leave, PacketHeader, TLV framing,
# version negotiation and constant-time token comparison.
# Deliberately not jamn_engine: the roster is a protocol-level table, not a
# scheduling one, and keeping that edge absent is what lets both modules be
# reasoned about separately.
target_link_libraries(jamn_session PUBLIC jamn_net jamn_proto)
target_compile_features(jamn_session PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_executable(jamn_session_tests
        tests/roster_tests.cpp
        tests/session_host_tests.cpp
    )
    target_link_libraries(jamn_session_tests PRIVATE jamn_session Catch2::Catch2WithMain)
    catch_discover_tests(jamn_session_tests PROPERTIES LABELS "fast")
endif()
