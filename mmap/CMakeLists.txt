add_library(logger_mmap STATIC
    mmap_region.cpp
    mmap_buffer.cpp
    mmap_store.cpp
)

add_library(logger::mmap ALIAS logger_mmap)

target_include_directories(logger_mmap PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_mmap PUBLIC cxx_std_17)

target_link_libraries(logger_mmap
    PUBLIC  logger::utils
            logger::context
)
