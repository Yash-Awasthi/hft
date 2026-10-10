add_executable(itch_fuzz itch_fuzz.cpp)
target_compile_options(itch_fuzz PRIVATE -fsanitize=fuzzer)
target_link_libraries(itch_fuzz PRIVATE hft_core -fsanitize=fuzzer)

add_executable(tape_fuzz tape_fuzz.cpp)
target_compile_options(tape_fuzz PRIVATE -fsanitize=fuzzer)
target_link_libraries(tape_fuzz PRIVATE hft_core -fsanitize=fuzzer)

add_executable(oms_fuzz oms_fuzz.cpp)
target_compile_options(oms_fuzz PRIVATE -fsanitize=fuzzer)
target_link_libraries(oms_fuzz PRIVATE hft_core -fsanitize=fuzzer)
