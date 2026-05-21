add_library(logger_context STATIC
    thread_pool.cpp
    strand.cpp
    context.cpp
)

add_library(logger::context ALIAS logger_context)

target_include_directories(logger_context PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(logger_context PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(logger_context
    PUBLIC  Threads::Threads
)
