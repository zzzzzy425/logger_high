find_package(fmt CONFIG REQUIRED)

add_library(logger_formatter STATIC
    default_formatter.cpp
)

add_library(logger::formatter ALIAS logger_formatter)

target_include_directories(logger_formatter PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_formatter PUBLIC cxx_std_17)

target_link_libraries(logger_formatter
    PUBLIC  logger::utils
    PRIVATE fmt::fmt
)
