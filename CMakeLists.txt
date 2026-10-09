cmake_minimum_required(VERSION 3.28)
project(hft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Werror)

option(HFT_FUZZ "Build libFuzzer targets (clang only)" OFF)
option(HFT_PYTHON "Build the nanobind research module" OFF)
if(HFT_PYTHON)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # static libraries are linked into the module
endif()

find_program(CCACHE ccache)
if(CCACHE)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(rapidcheck CONFIG REQUIRED)
find_package(hdr_histogram CONFIG REQUIRED)
find_package(tlx CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(zlib-ng CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE src)

add_library(hft_data STATIC src/data/store.cpp src/data/gzip.cpp)
target_link_libraries(hft_data PUBLIC hft_core zstd::libzstd lz4::lz4 zlib-ng::zlib Threads::Threads)

add_library(hft_experiment STATIC src/experiment/config.cpp src/experiment/splits.cpp
                                  src/experiment/registry.cpp)
target_link_libraries(hft_experiment PUBLIC hft_core tomlplusplus::tomlplusplus
                                            unofficial::sqlite3::sqlite3)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
add_subdirectory(apps)
if(HFT_PYTHON)
  find_package(Python 3.12 COMPONENTS Interpreter Development.Module REQUIRED)
  execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
                  OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
  find_package(nanobind CONFIG REQUIRED)
  nanobind_add_module(hftpy NB_STATIC bindings/hftpy.cpp)
  target_link_libraries(hftpy PRIVATE hft_core hft_data)
endif()
if(HFT_FUZZ)
  add_subdirectory(fuzz)
endif()
