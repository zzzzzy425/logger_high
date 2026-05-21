add_library(logger_sinks STATIC
    console_sink.cpp
)

add_library(logger::sinks ALIAS logger_sinks)

target_include_directories(logger_sinks PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_sinks PUBLIC cxx_std_17)

target_link_libraries(logger_sinks
    PUBLIC  logger::formatter
)
