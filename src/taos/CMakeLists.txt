# tdx_taos：TDengine 导入层
# RAII 封装 + STMT 批量绑定 + 多线程导入编排
find_library(TAOS_LIBRARY
  NAMES taos
  PATHS /lib /lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
        /usr/local/taos/lib /usr/local/taos/lib64 /usr/local/taos/driver
  REQUIRED
)
message(STATUS "tdx-cpp: 找到 TDengine 库 ${TAOS_LIBRARY}")

add_library(tdx_taos STATIC
  taos_connection.cpp
  taos_import.cpp
)
target_include_directories(tdx_taos
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
  PRIVATE /usr/local/taos/include
)
target_link_libraries(tdx_taos
  PUBLIC  tdx_proto_local tdx_quotes
  PUBLIC  ${TAOS_LIBRARY} dl pthread
)
