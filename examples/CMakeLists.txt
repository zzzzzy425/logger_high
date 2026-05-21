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
