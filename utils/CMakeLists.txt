add_library(logger_utils STATIC
    system_info.cpp
)

add_library(logger::utils ALIAS logger_utils)

target_include_directories(logger_utils PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_utils PUBLIC cxx_std_17)
