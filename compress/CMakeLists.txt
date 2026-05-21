find_package(ZLIB REQUIRED)

add_library(logger_compress STATIC
    zlib_compressor.cpp
)

add_library(logger::compress ALIAS logger_compress)

target_include_directories(logger_compress PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_compress PUBLIC cxx_std_17)

target_link_libraries(logger_compress
    PRIVATE ZLIB::ZLIB
)
