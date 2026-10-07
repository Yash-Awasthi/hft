add_executable(hft_bench endian_bench.cpp fork_bench.cpp philox_bench.cpp transformer_bench.cpp)
target_compile_definitions(hft_bench PRIVATE HFT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(hft_bench PRIVATE hft_core benchmark::benchmark_main)

add_executable(book_study book_study.cpp)
target_link_libraries(book_study PRIVATE hft_core hft_data hdr_histogram::hdr_histogram_static tlx)
