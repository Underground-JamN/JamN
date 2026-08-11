add_library(jamn_proto INTERFACE)
target_include_directories(jamn_proto INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(jamn_proto INTERFACE cxx_std_20)
