# tdx_shm：盘中实时行情共享内存（裸 mmap + seqlock 快照表 + 时序 ring）
# 纯标准库（无 helio/Boost 依赖），跨进程共享定长 POD 行情快照。
# 设计文档：.claude/PRPs/prds/phase6-intraday-shm-design.md
add_library(tdx_shm STATIC
  segment.cpp
  snapshot.cpp
)
target_include_directories(tdx_shm PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tdx_shm PUBLIC pthread)
