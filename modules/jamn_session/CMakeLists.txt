add_library(jamn_session INTERFACE)
target_include_directories(jamn_session INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(jamn_session INTERFACE cxx_std_20)
