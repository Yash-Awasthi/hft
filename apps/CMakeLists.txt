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
