add_executable(hello hello.cpp)

target_link_libraries(hello PRIVATE
    logger::handle
)

add_executable(context_smoke context_smoke.cpp)

target_link_libraries(context_smoke PRIVATE
    logger::context
)

add_executable(mmap_smoke mmap_smoke.cpp)

target_link_libraries(mmap_smoke PRIVATE
    logger::mmap
    logger::context
)

add_executable(compress_smoke compress_smoke.cpp)

target_link_libraries(compress_smoke PRIVATE
    logger::compress
)

add_executable(crypt_smoke crypt_smoke.cpp)

# crypt_smoke uses cryptopp directly in case6 to simulate the audit party.
find_package(cryptopp CONFIG REQUIRED)
target_link_libraries(crypt_smoke PRIVATE
    logger::crypt
    cryptopp::cryptopp
)

add_executable(proto_smoke proto_smoke.cpp)

target_link_libraries(proto_smoke PRIVATE
    logger::formatter
    logger::proto
    logger::utils
)

add_executable(effective_sink_smoke effective_sink_smoke.cpp)

find_package(cryptopp CONFIG REQUIRED)
target_link_libraries(effective_sink_smoke PRIVATE
    logger::sinks
    logger::context
    logger::mmap
    logger::compress
    logger::crypt
    logger::formatter
    logger::proto
    logger::utils
    cryptopp::cryptopp
)

add_executable(log_decoder log_decoder.cpp)

target_link_libraries(log_decoder PRIVATE
    logger::audit
    logger::utils
)

add_executable(encrypted_logging_demo encrypted_logging_demo.cpp)

target_link_libraries(encrypted_logging_demo PRIVATE
    logger::handle
    logger::sinks
    logger::context
    logger::audit
    logger::utils
    cryptopp::cryptopp
)
