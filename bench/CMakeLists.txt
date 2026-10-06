add_executable(hft_bench endian_bench.cpp)
target_link_libraries(hft_bench PRIVATE hft_core benchmark::benchmark_main)
