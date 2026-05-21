add_library(logger_handle STATIC
    log_handle.cpp
)

add_library(logger::handle ALIAS logger_handle)

target_include_directories(logger_handle PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_handle PUBLIC cxx_std_17)

target_link_libraries(logger_handle
    PUBLIC logger::sinks
)
