add_library(jamn_engine INTERFACE)
target_include_directories(jamn_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(jamn_engine INTERFACE cxx_std_20)
