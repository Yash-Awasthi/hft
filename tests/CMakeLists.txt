include(GoogleTest)

add_executable(hft_tests endian_test.cpp itch_test.cpp store_test.cpp gzip_test.cpp book_test.cpp
                         book_prop_test.cpp)
target_link_libraries(hft_tests PRIVATE hft_core hft_data GTest::gtest_main rapidcheck)
gtest_discover_tests(hft_tests)
# RapidCheck instantiates the deprecated std::aligned_storage in user translation units.
set_source_files_properties(book_prop_test.cpp PROPERTIES COMPILE_OPTIONS -Wno-deprecated-declarations)

add_executable(alloc_test alloc_test.cpp)
target_link_libraries(alloc_test PRIVATE hft_core GTest::gtest_main)
gtest_discover_tests(alloc_test)
