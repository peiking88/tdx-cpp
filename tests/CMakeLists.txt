# tdx-cpp 测试
# 测试链接 helio FetchContent 提供的 gtest（不用系统 /usr/include/gtest，避免同名 target 冲突）。
# 测试代码豁免 fiber 纪律（评审决策 #3：gtest 跑在 Proactor 线程外）。
set(TDX_TEST_LIBS tdx_proto gtest gtest_main)

function(tdx_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(${name} PRIVATE ${TDX_TEST_LIBS})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# 需 helio ProactorPool 编排的测试：custom main（含 MainInitGuard），链 base/fibers2。
function(tdx_test_helio name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(${name} PRIVATE tdx_proto gtest base fibers2)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# 数据管理层测试：链 tdx_data（calendar/adjust/resampler/sync/hybrid）
function(tdx_test_data name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(${name} PRIVATE tdx_data gtest gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# DuckDB 查询层测试：链 tdx_query（duckdb IMPORTED，无 Arrow）
function(tdx_test_query name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(${name} PRIVATE tdx_query gtest gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

tdx_test(test_codec)
tdx_test(test_frame)
tdx_test(test_util)
tdx_test(test_parsers)
tdx_test(test_vipdoc_reader)
tdx_test(test_golden)
tdx_test(test_sp_codec)
tdx_test(test_json)
tdx_test(test_phase2)
tdx_test_data(test_phase3)
# test_sync4 用 custom main（MainInitGuard，SyncState fb2::Mutex 需 helio init）
add_executable(test_sync4 test_sync4.cpp)
target_include_directories(test_sync4 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_sync4 PRIVATE tdx_data base gtest)
add_test(NAME test_sync4 COMMAND test_sync4)
tdx_test_query(test_duckdb)
tdx_test_helio(test_retry)
tdx_test_helio(test_server_pool)

# golden 测试需 fixtures 路径
target_compile_definitions(test_golden PRIVATE
    FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/golden")
# Phase 3 测试需 cfg 路径（holidays.json）
target_compile_definitions(test_phase3 PRIVATE
    CFG_DIR="${CMAKE_SOURCE_DIR}/cfg")
