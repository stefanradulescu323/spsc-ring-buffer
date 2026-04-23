cmake_minimum_required(VERSION 3.20)
project(spsc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC-appropriate flags
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options($<$<CONFIG:Release>:-O3>
                       $<$<CONFIG:Release>:-march=native>)
endif()

# Header-only library
add_library(spsc INTERFACE)
target_include_directories(spsc INTERFACE include)

# Benchmark executable
add_executable(benchmark benchmark/benchmark.cpp)
target_link_libraries(benchmark PRIVATE spsc)

# Test executable
add_executable(test_basic tests/test_basic.cpp)
target_link_libraries(test_basic PRIVATE spsc)