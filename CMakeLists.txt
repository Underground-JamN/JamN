cmake_minimum_required(VERSION 3.25)
# C is enabled here, not just CXX, because ENet (fetched below) is a C
# project; enabling C for the first time from a FetchContent subdirectory
# instead of the top-level project() call breaks Ninja's generate step.
project(JamN VERSION 0.3 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# When ON, configure only the six JUCE-free modules and their tests: no
# ENet, no JUCE, no network egress beyond Catch2 itself. This is what proves
# the module split is real rather than aspirational (see AGENTS.md's Modules
# section).
option(JAMN_CORE_ONLY "Build only the JUCE-free, network-free modules" OFF)

include(FetchContent)
include(CTest)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        95d8a61b089317bec800c7cc4c64064cbcb3802d # v3.15.3
    GIT_SHALLOW    OFF
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_subdirectory(modules/jamn_core)
add_subdirectory(modules/jamn_proto)
add_subdirectory(modules/jamn_net)
add_subdirectory(modules/jamn_dsp)
add_subdirectory(modules/jamn_engine)
add_subdirectory(modules/jamn_session)

# jamn_bench only needs jamn_core and jamn_engine so far, not JUCE - keep
# it building under core-only too until it grows a real jamn_platform
# backend.
add_subdirectory(tools/jamn_bench)

if(NOT JAMN_CORE_ONLY)
    FetchContent_Declare(
        ENet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG        2662c0de09e36f2a2030ccc2c528a3e4c9e8138a # v1.3.18
        GIT_SHALLOW    OFF
    )
    FetchContent_MakeAvailable(ENet)

    # ENet exports no usable include path of its own: its CMakeLists.txt
    # uses a directory-scoped include_directories() with no
    # target_include_directories and no ALIAS target, so linking `enet`
    # alone would not resolve #include <enet/enet.h>. Attach it here rather
    # than hardcoding the path inside jamn_net, so the fetch stays the one
    # place that knows where ENet's source landed.
    target_include_directories(enet INTERFACE ${enet_SOURCE_DIR}/include)

    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        f72bad64d29715216226685810c5196bd0d79d77 # 8.0.9
        GIT_SHALLOW    OFF
    )
    FetchContent_MakeAvailable(JUCE)

    # webkit2gtk is absent on the Linux dev box; these must stay off in
    # every build, not just be "not installed yet". These
    # are juce_core.h "Config:" macros, not real JUCE CMake cache options
    # (JUCE_WEB_BROWSER/JUCE_USE_CURL cache variables do nothing in JUCE 8's
    # CMake) - they only take effect as compile definitions on juce_core
    # itself, propagated INTERFACE to every module that links it.
    target_compile_definitions(juce_core INTERFACE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )

    add_subdirectory(modules/jamn_platform)
    add_subdirectory(modules/jamn_ui)
    add_subdirectory(modules/jamn_app)
endif()
