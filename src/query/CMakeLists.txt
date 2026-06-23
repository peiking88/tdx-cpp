# DuckDB 查询层（Parquet 读写 + SQL + 内存热表，无 Arrow）
# 注意：vendored libduckdb.so 用旧 ABI（_GLIBCXX_USE_CXX11_ABI=0），tdx_query 须匹配
add_library(tdx_query STATIC duckdb_query.cpp)
target_include_directories(tdx_query PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_query PUBLIC tdx_proto duckdb dl)
target_compile_definitions(tdx_query PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
