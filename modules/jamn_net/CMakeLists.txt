add_library(jamn_net INTERFACE)
target_include_directories(jamn_net INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(jamn_net INTERFACE cxx_std_20)
