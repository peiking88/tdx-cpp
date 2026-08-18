# czsc：缠论分析引擎（并入自 czsc-cpp v0.7.0，upstream ebb3ef2，仓库已冻结为上游归档）
# 分层: czsc_types → czsc_ta / czsc_analyze → czsc_signals → czsc_io / czsc_signals_persist
# 与 tdx-cpp 的整合约定:
#   - nlohmann/json 用本仓 vendored include/nlohmann（同 3.11.3），不再 FetchContent
#   - gtest 复用 helio FetchContent 提供的 target（tests/ 内注册）
#   - 不链 helio/absl —— 保持纯计算库属性；taos 连接走 tdx_taos_conn（RAII 叶子）

find_library(CZSC_TA_LIB
  NAMES ta-lib
  PATHS /lib /lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
  REQUIRED)
message(STATUS "czsc: ta-lib = ${CZSC_TA_LIB}")

# ---- czsc_types：基础类型 + JSON 序列化 ----
add_library(czsc_types STATIC
    types/raw_bar.cpp
    types/new_bar.cpp
    types/fx.cpp
    types/bi.cpp
    types/zs.cpp
    types/signal.cpp
)
target_include_directories(czsc_types PUBLIC ${CMAKE_SOURCE_DIR}/include)

# ---- czsc_ta：TA 算子层（ta-lib thin wrappers + 增量缓存）----
add_library(czsc_ta STATIC
    ta/ta_wrapper.cpp
    ta/ta_cache.cpp
)
target_include_directories(czsc_ta PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(czsc_ta PUBLIC czsc_types ${CZSC_TA_LIB})

# ---- czsc_analyze：缠论核心（包含关系/分型/笔/中枢增量引擎）----
add_library(czsc_analyze STATIC
    analyze/algorithms.cpp
    analyze/czsc.cpp
)
target_include_directories(czsc_analyze PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(czsc_analyze PUBLIC czsc_types)

# ---- czsc_signals：信号基础设施 + 246 信号函数 ----
add_library(czsc_signals STATIC
    signals/param_view.cpp
    signals/signal_builder.cpp
    signals/registry.cpp
)
target_include_directories(czsc_signals PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(czsc_signals PUBLIC czsc_analyze czsc_ta)

# ---- czsc_io：数据加载（TDengine 日线 + mmap 实时快照拼接）----
# shm 直链 tdx_shm（原 czsc-cpp TDX_CPP_DIR 反向引用已解除）；
# taos 连接统一走 tdx_taos_conn（tdx::taos::TaosConnection RAII）。
add_library(czsc_io STATIC
    io/data_loader.cpp
)
target_include_directories(czsc_io PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(czsc_io PUBLIC czsc_types tdx_shm tdx_taos_conn pthread)

# ---- czsc_signals_persist：信号持久化（TDengine signals 表读写）----
add_library(czsc_signals_persist STATIC
    io/signal_store.cpp
)
target_include_directories(czsc_signals_persist PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(czsc_signals_persist PUBLIC czsc_io)
