include(GoogleTest)

add_executable(hft_tests endian_test.cpp itch_test.cpp store_test.cpp gzip_test.cpp book_test.cpp
                         book_prop_test.cpp timing_test.cpp philox_test.cpp
                         matching_test.cpp scheduler_test.cpp
                         replay_exchange_test.cpp experiment_test.cpp
                         fixture_test.cpp features_test.cpp accounting_test.cpp
                         backtest_test.cpp queue_reactive_test.cpp tick_stats_test.cpp
                         trades_test.cpp event_tokens_test.cpp
                         event_transformer_test.cpp intensity_test.cpp net_test.cpp pm_test.cpp)
target_link_libraries(hft_tests PRIVATE hft_core hft_data GTest::gtest_main rapidcheck tlx
                                        hft_experiment zstd::libzstd OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(hft_tests PRIVATE HFT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(hft_tests)
# RapidCheck instantiates the deprecated std::aligned_storage in user translation units.
set_source_files_properties(book_prop_test.cpp matching_test.cpp PROPERTIES COMPILE_OPTIONS -Wno-deprecated-declarations)

# Sanitizer runtimes define their own operator new, which the counting allocator replaces.
if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  add_executable(alloc_test alloc_test.cpp)
  target_link_libraries(alloc_test PRIVATE hft_core GTest::gtest_main)
  gtest_discover_tests(alloc_test)
endif()
