include(GoogleTest)

add_executable(hft_tests endian_test.cpp)
target_link_libraries(hft_tests PRIVATE hft_core GTest::gtest_main)
gtest_discover_tests(hft_tests)
