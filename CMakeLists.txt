cmake_minimum_required(VERSION 3.21)
project(logger_high
    VERSION 0.1.0
    DESCRIPTION "High-performance C++ logging library"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(utils)
add_subdirectory(context)
add_subdirectory(mmap)
add_subdirectory(compress)
add_subdirectory(crypt)
add_subdirectory(proto)
add_subdirectory(formatter)
add_subdirectory(sinks)
add_subdirectory(handle)
add_subdirectory(audit)
add_subdirectory(examples)
