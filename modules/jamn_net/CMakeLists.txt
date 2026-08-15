# STATIC, not INTERFACE, now that SimTransport needs a .cpp - same
# reasoning as modules/jamn_dsp/CMakeLists.txt.
add_library(jamn_net STATIC
    src/sim_transport.cpp
)
target_include_directories(jamn_net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# jamn_net -> jamn_core: SimClock/SessionTime and ByteReader.
target_link_libraries(jamn_net PUBLIC jamn_core)
target_compile_features(jamn_net PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_executable(jamn_net_tests
        tests/sim_transport_tests.cpp
    )
    target_link_libraries(jamn_net_tests PRIVATE jamn_net Catch2::Catch2WithMain)
    catch_discover_tests(jamn_net_tests PROPERTIES LABELS "fast")
endif()

# EnetTransport, and only EnetTransport, links ENet. The guard must be
# JAMN_CORE_ONLY and never an "is the enet target defined yet" test:
# add_subdirectory(modules/jamn_net) runs before the root CMakeLists.txt's
# ENet fetch, so such a test is evaluated immediately and is false in *both*
# presets - it would silently produce a full build with no EnetTransport in
# it while the gate still went green. target_link_libraries by contrast
# resolves at generate time, so naming a target that does not exist yet is
# fine. The include path comes from the INTERFACE property the root
# CMakeLists.txt attaches to enet after FetchContent_MakeAvailable; ENet
# exports none of its own.
if(NOT JAMN_CORE_ONLY)
    target_sources(jamn_net PRIVATE src/enet_transport.cpp)
    target_link_libraries(jamn_net PRIVATE enet)

    if(BUILD_TESTING)
        # A third label, `net`: these bind real sockets, so they cannot
        # carry `fast` - that list must be identical under core-only (where
        # ENet is never fetched) and the full build, and both gate scripts
        # prove it by running -L fast twice.
        add_executable(jamn_net_enet_tests
            tests/enet_transport_tests.cpp
        )
        target_link_libraries(jamn_net_enet_tests PRIVATE jamn_net Catch2::Catch2WithMain)
        catch_discover_tests(jamn_net_enet_tests PROPERTIES LABELS "net")
    endif()
endif()
