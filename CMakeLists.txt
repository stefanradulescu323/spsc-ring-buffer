cmake_minimum_required(VERSION 3.20)
project(spsc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build; otherwise CMake builds with no -O flag at all
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    # Release already implies -O3 -DNDEBUG; add CPU-specific instructions
    add_compile_options($<$<CONFIG:Release>:-march=native>)
endif()

option(SPSC_TSAN "Build with ThreadSanitizer (GCC/Clang only)" OFF)
if(SPSC_TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

# Header-only library
add_library(spsc INTERFACE)
target_include_directories(spsc INTERFACE include)
target_link_libraries(spsc INTERFACE Threads::Threads)

add_executable(benchmark benchmark/benchmark.cpp)
target_link_libraries(benchmark PRIVATE spsc)

add_executable(test_basic tests/test_basic.cpp)
target_link_libraries(test_basic PRIVATE spsc)