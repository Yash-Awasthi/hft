add_executable(itch_fuzz itch_fuzz.cpp)
target_compile_options(itch_fuzz PRIVATE -fsanitize=fuzzer)
target_link_libraries(itch_fuzz PRIVATE hft_core -fsanitize=fuzzer)
