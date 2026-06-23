# tdx-cpp 源码层
# 分层（依赖自下而上）：tdx_util → tdx_proto → tdx_quotes → tdx_cli

# ---- tdx_util：基础工具（iconv GBK 转码、zlib 解压、字节序、时间）----
add_library(tdx_util STATIC
    util/gbk.cpp
    util/zlib_wrap.cpp
    util/time_util.cpp
)
target_include_directories(tdx_util PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_util PUBLIC ZLIB::ZLIB)
# iconv：glibc 内置（libc），Linux 无需额外 link。

# ---- tdx_proto：协议层（帧编解码 → codec → connection → 弹性 → Parser → vipdoc）----
# 后续 task 逐步追加源文件。connection 起 fiber 内代码，需 fibers2。
add_library(tdx_proto STATIC
    proto/frame.cpp
    proto/codec.cpp
    proto/connection.cpp
    proto/retry.cpp
    proto/heartbeat.cpp
    proto/server_pool.cpp
    proto/parsers.cpp
    proto/parsers_quotes.cpp
    proto/vipdoc_reader.cpp
    proto/sp_codec.cpp
    proto/ex_parsers.cpp
    proto/bitmap.cpp
    proto/sp_parsers.cpp
    proto/block_reader.cpp
)
target_link_libraries(tdx_proto PUBLIC tdx_util fibers2)

# ---- tdx_quotes：行情接口层（StdQuotes 组合协议层）----
add_library(tdx_quotes STATIC
    quotes/std_quotes.cpp
    quotes/ext_quotes.cpp
    quotes/sp_quotes.cpp
)
target_link_libraries(tdx_quotes PUBLIC tdx_proto)

# ---- tdx_data：数据管理层（Phase 3）----
add_subdirectory(data)

# ---- tdx_query：DuckDB 查询层（Phase 4，Parquet 读写+SQL，无 Arrow）----
add_subdirectory(query)

# ---- tdx_batch：并发批量拉取（Phase 4，helio fiber 分片）----
add_subdirectory(batch)

# ---- Task 1 smoke：验证 helio 集成 ----
add_executable(tdx_smoke smoke.cc)
target_link_libraries(tdx_smoke PRIVATE base fibers2)

# ---- tdx_cli：命令行工具（server-test / bars / ex-bars / fetch-history）----
add_executable(tdx cli/main.cpp)
target_link_libraries(tdx PRIVATE tdx_data tdx_query tdx_batch tdx_quotes base fibers2)
