# tdx-cpp 源码层
# 分层（依赖自下而上）：tdx_util → tdx_proto → tdx_quotes → tdx_cli

# ---- tdx_util：基础工具（GBK iconv、zlib 解压、absl::Time 时区）----
add_library(tdx_util STATIC
    util/gbk.cpp
    util/zlib_wrap.cpp
    util/time_util.cpp
)
target_include_directories(tdx_util PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_util PUBLIC zlib absl::time absl::time_zone)
# iconv：glibc 内置（libc），Linux 无需额外 link。

# ---- tdx_proto：协议层（4 子 target + umbrella）----
# core: 帧编解码（无 fiber 依赖，纯协议基础）
add_library(tdx_proto_core STATIC
    proto/frame.cpp
    proto/codec.cpp
)
target_include_directories(tdx_proto_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_proto_core PUBLIC tdx_util)

# transport: TCP 连接 + 心跳 + 重试 + 连接池（需 fibers2）
add_library(tdx_proto_transport STATIC
    proto/connection.cpp
    proto/heartbeat.cpp
    proto/retry.cpp
    proto/server_pool.cpp
)
target_include_directories(tdx_proto_transport PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_proto_transport PUBLIC tdx_proto_core fibers2)

# parsers: 标准/扩展/SP 协议解析（变长价格 + K线/分时/逐笔/五档/列表/板块/资金流）
add_library(tdx_proto_parsers STATIC
    proto/parsers.cpp
    proto/parsers_quotes.cpp
    proto/ex_parsers.cpp
    proto/sp_codec.cpp
    proto/sp_parsers.cpp
    proto/bitmap.cpp
)
target_include_directories(tdx_proto_parsers PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_proto_parsers PUBLIC tdx_proto_core)

# local: 本地 vipdoc 文件读取（.day/.lc1/.lc5/block.dat）
add_library(tdx_proto_local STATIC
    proto/vipdoc_reader.cpp
    proto/block_reader.cpp
)
target_include_directories(tdx_proto_local PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_proto_local PUBLIC tdx_proto_core)

# umbrella: tdx_proto（向后兼容，聚合 4 子 target）
add_library(tdx_proto INTERFACE)
target_link_libraries(tdx_proto INTERFACE tdx_proto_core tdx_proto_transport tdx_proto_parsers tdx_proto_local)

# ---- tdx_quotes：行情接口层（StdQuotes 组合协议层）----
add_library(tdx_quotes STATIC
    quotes/std_quotes.cpp
    quotes/ext_quotes.cpp
    quotes/sp_quotes.cpp
)
target_link_libraries(tdx_quotes PUBLIC tdx_proto)

# ---- tdx_data：数据管理层（Phase 3）----
add_subdirectory(data)

# ---- tdx_batch：并发批量拉取（Phase 4，helio fiber 分片）----
add_subdirectory(batch)

# ---- tdx_taos：TDengine 导入层（多线程 + STMT 批量绑定）----
add_subdirectory(taos)

# ---- tdx_shm：盘中实时行情共享内存（Phase 6，纯标准库 mmap + seqlock）----
add_subdirectory(shm)

# ---- Task 1 smoke：验证 helio 集成（输出到 build/demos）----
add_executable(tdx_smoke smoke.cc)
set_target_properties(tdx_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/demos)
target_link_libraries(tdx_smoke PRIVATE base fibers2)

# ---- tdx_cli：命令行工具（server-test / bars / ex-bars / fetch-history / import）----
add_executable(tdx cli/main.cpp cli/import.cpp cli/fetch_quotes.cpp)
target_link_libraries(tdx PRIVATE tdx_data tdx_batch tdx_quotes tdx_taos tdx_shm base fibers2)

# ---- tdx_quotes_reader：盘中实时行情共享内存只读查看器（Phase 6）----
add_executable(tdx_quotes_reader cli/quotes_reader.cpp)
target_link_libraries(tdx_quotes_reader PRIVATE tdx_shm)
