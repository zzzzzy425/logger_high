find_package(cryptopp CONFIG REQUIRED)

add_library(logger_audit STATIC
    shard_reader.cpp
)

add_library(logger::audit ALIAS logger_audit)

target_include_directories(logger_audit PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_audit PUBLIC cxx_std_17)

target_link_libraries(logger_audit
    PUBLIC  logger::proto
    PRIVATE logger::crypt
            logger::compress
            logger::sinks
            cryptopp::cryptopp
)
