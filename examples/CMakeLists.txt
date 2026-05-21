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
