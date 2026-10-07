add_executable(itch_count itch_count.cpp)
target_link_libraries(itch_count PRIVATE hft_core)

add_executable(ingest ingest.cpp)
target_link_libraries(ingest PRIVATE hft_core hft_data)

add_executable(store_cat store_cat.cpp)
target_link_libraries(store_cat PRIVATE hft_core hft_data)

add_executable(book_replay book_replay.cpp)
target_link_libraries(book_replay PRIVATE hft_core hft_data tlx)

add_executable(checkpoint checkpoint.cpp)
target_link_libraries(checkpoint PRIVATE hft_core hft_data)

add_executable(itch_dump itch_dump.cpp)
target_link_libraries(itch_dump PRIVATE hft_core)

add_executable(gen_fixture gen_fixture.cpp)
target_link_libraries(gen_fixture PRIVATE hft_core zstd::libzstd)

add_executable(day_stats day_stats.cpp)
target_link_libraries(day_stats PRIVATE hft_core hft_data)

add_executable(feature_stream feature_stream.cpp)
target_link_libraries(feature_stream PRIVATE hft_core hft_data)
