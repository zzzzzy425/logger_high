find_package(fmt CONFIG REQUIRED)

add_library(logger_utils STATIC
    system_info.cpp
    internal_log.cpp
)

add_library(logger::utils ALIAS logger_utils)

target_include_directories(logger_utils PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_utils PUBLIC cxx_std_17)

target_link_libraries(logger_utils
    PUBLIC fmt::fmt
)
