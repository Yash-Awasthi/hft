cmake_minimum_required(VERSION 3.28)
project(hft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules here; the default dependency scan adds a dyndep step per target that ninja
# 1.13 can assert on.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Werror)

option(HFT_FUZZ "Build libFuzzer targets (clang only)" OFF)

find_program(CCACHE ccache)
if(CCACHE)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(rapidcheck CONFIG REQUIRED)

find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(zlib-ng CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE src)

add_library(hft_data STATIC src/data/store.cpp src/data/gzip.cpp)
target_link_libraries(hft_data PUBLIC hft_core zstd::libzstd lz4::lz4 zlib-ng::zlib Threads::Threads)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
add_subdirectory(apps)
if(HFT_FUZZ)
  add_subdirectory(fuzz)
endif()
