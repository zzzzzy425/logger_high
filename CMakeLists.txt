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

# Phase 0: only utils is built. Other subdirs (handle/, sinks/, ...) will be
# wired in as their first sources land. See memory/roadmap.md.
add_subdirectory(utils)
