# tdx_taos：TDengine 导入层
# RAII 封装 + STMT 批量绑定 + 多线程导入编排
find_library(TAOS_LIBRARY
  NAMES taos
  PATHS /lib /lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
        /usr/local/taos/lib /usr/local/taos/lib64 /usr/local/taos/driver
  REQUIRED
)
message(STATUS "tdx-cpp: 找到 TDengine 库 ${TAOS_LIBRARY}")

# tdx_taos_conn：连接 RAII 叶子（仅 taos C API，不拖 proto/quotes/helio）。
# czsc 等纯计算模块复用同一连接实现（统一连接管理，零 helio 依赖）。
add_library(tdx_taos_conn STATIC
  taos_connection.cpp
)
target_include_directories(tdx_taos_conn
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
  PRIVATE /usr/local/taos/include
)
target_link_libraries(tdx_taos_conn
  PUBLIC  ${TAOS_LIBRARY} dl pthread
)

add_library(tdx_taos STATIC
  taos_import.cpp
)
target_include_directories(tdx_taos
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
  PRIVATE /usr/local/taos/include
)
target_link_libraries(tdx_taos
  PUBLIC  tdx_taos_conn tdx_proto_local tdx_quotes
  PUBLIC  ${TAOS_LIBRARY} dl pthread
)
