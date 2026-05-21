find_package(cryptopp CONFIG REQUIRED)

add_library(logger_crypt STATIC
    aes_gcm_cipher.cpp
    x25519_agreement.cpp
    kdf.cpp
    server_key_loader.cpp
)

add_library(logger::crypt ALIAS logger_crypt)

target_include_directories(logger_crypt PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_crypt PUBLIC cxx_std_17)

target_link_libraries(logger_crypt
    PRIVATE cryptopp::cryptopp
)
