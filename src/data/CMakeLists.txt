# tdx_data：数据管理层（Phase 3，对齐 tdxdata）
# 复用 tdx_proto（types/consts/util）+ vendored nlohmann/json（include/nlohmann/）
# tdx_data.hpp 公开依赖 tdx_quotes + tdx_query（TdxData 组合 StdQuotes + DuckDBQuery），故 PUBLIC。
add_library(tdx_data STATIC
    calendar.cpp
    adjust.cpp
    resampler.cpp
    sync_state.cpp
    tdx_data.cpp
)
target_include_directories(tdx_data PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_data PUBLIC tdx_quotes tdx_query)
