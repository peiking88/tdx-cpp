# tdx_data：数据管理层（Phase 3，对齐 tdxdata）
# 复用 tdx_proto（types/consts/util）+ vendored nlohmann/json
add_library(tdx_data STATIC
    calendar.cpp
    adjust.cpp
    resampler.cpp
    sync_state.cpp
    tdx_data.cpp
)
target_include_directories(tdx_data PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_data PUBLIC tdx_quotes)
