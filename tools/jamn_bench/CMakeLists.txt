add_library(jamn_bench_lib STATIC
    src/bench_result.cpp
)
target_include_directories(jamn_bench_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(jamn_bench_lib PUBLIC cxx_std_20)

add_executable(jamn_bench src/main.cpp)
target_link_libraries(jamn_bench PRIVATE jamn_bench_lib jamn_core)
target_compile_features(jamn_bench PRIVATE cxx_std_20)

if(BUILD_TESTING)
    add_executable(jamn_bench_tests
        tests/bench_result_tests.cpp
    )
    target_link_libraries(jamn_bench_tests PRIVATE jamn_bench_lib Catch2::Catch2WithMain)
    catch_discover_tests(jamn_bench_tests PROPERTIES LABELS "fast")
endif()
