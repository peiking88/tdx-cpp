# 并发批量拉取（helio fiber 分片）
add_library(tdx_batch STATIC batch_fetch.cpp)
target_include_directories(tdx_batch PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_batch PUBLIC tdx_quotes)
