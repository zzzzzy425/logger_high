add_library(logger_sinks STATIC
    console_sink.cpp
    shard_writer.cpp
    eviction_runner.cpp
    effective_sink.cpp
)

add_library(logger::sinks ALIAS logger_sinks)

target_include_directories(logger_sinks PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_sinks PUBLIC cxx_std_17)

target_link_libraries(logger_sinks
    PUBLIC  logger::formatter
            logger::mmap
            logger::context
    PRIVATE logger::compress
            logger::crypt
            logger::utils
            logger::proto
)
