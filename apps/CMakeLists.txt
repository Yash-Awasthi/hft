add_executable(itch_count itch_count.cpp)
target_link_libraries(itch_count PRIVATE hft_core)

add_executable(ingest ingest.cpp)
target_link_libraries(ingest PRIVATE hft_core hft_data)

add_executable(store_cat store_cat.cpp)
target_link_libraries(store_cat PRIVATE hft_core hft_data)
