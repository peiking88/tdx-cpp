# tdx-cpp 测试
# 测试链接 helio FetchContent 提供的 gtest（不用系统 /usr/include/gtest，避免同名 target 冲突）。
# 测试代码豁免 fiber 纪律（评审决策 #3：gtest 跑在 Proactor 线程外）。
# 测试可执行文件输出到 build/tests（与 CLI/demos 分离）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
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

# 精细链接：仅链接所需子 target，避免冗余 fibers2 依赖
add_executable(test_codec test_codec.cpp)
target_include_directories(test_codec PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_codec PRIVATE tdx_proto_core gtest gtest_main)
add_test(NAME test_codec COMMAND test_codec)

add_executable(test_frame test_frame.cpp)
target_include_directories(test_frame PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_frame PRIVATE tdx_proto_core gtest gtest_main)
add_test(NAME test_frame COMMAND test_frame)

add_executable(test_util test_util.cpp)
target_include_directories(test_util PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_util PRIVATE tdx_util gtest gtest_main)
add_test(NAME test_util COMMAND test_util)

# 需要 parsers 的测试：core + parsers
add_executable(test_parsers test_parsers.cpp)
target_include_directories(test_parsers PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_parsers PRIVATE tdx_proto_parsers tdx_proto_core gtest gtest_main)
add_test(NAME test_parsers COMMAND test_parsers)

# bars / ex-bars 真网测试（原 CLI 命令回退为测试用例）
add_executable(test_bars test_bars.cpp)
target_include_directories(test_bars PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_bars PRIVATE tdx_quotes tdx_proto_parsers tdx_proto_core gtest gtest_main base fibers2)
# 不可达时 SetUpTestSuite 用 GTEST_SKIP（不视为失败）→ 经 wrapper 把「全 SKIP」
# 翻译成 ctest SKIPPED（SKIP_RETURN_CODE=77），避免退出码 0 导致的假绿。
# 见 scripts/run_gtest.sh。
add_test(NAME test_bars
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_gtest.sh $<TARGET_FILE:test_bars>)
set_tests_properties(test_bars PROPERTIES SKIP_RETURN_CODE 77)

# fetch-history 真网测试（原 CLI 命令回退为测试用例）
add_executable(test_fetch_history test_fetch_history.cpp)
target_include_directories(test_fetch_history PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_fetch_history PRIVATE tdx_data gtest gtest_main base fibers2)
# 同 test_bars：不可达时全 SKIP → wrapper 翻译为 ctest SKIPPED，避免假绿。
add_test(NAME test_fetch_history
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_gtest.sh $<TARGET_FILE:test_fetch_history>)
set_tests_properties(test_fetch_history PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_sp_codec test_sp_codec.cpp)
target_include_directories(test_sp_codec PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_sp_codec PRIVATE tdx_proto_parsers tdx_proto_core gtest gtest_main)
add_test(NAME test_sp_codec COMMAND test_sp_codec)

add_executable(test_json test_json.cpp)
target_include_directories(test_json PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_json PRIVATE tdx_proto_parsers tdx_proto_core gtest gtest_main)
add_test(NAME test_json COMMAND test_json)

add_executable(test_phase2 test_phase2.cpp)
target_include_directories(test_phase2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_phase2 PRIVATE tdx_proto_parsers tdx_proto_core gtest gtest_main)
add_test(NAME test_phase2 COMMAND test_phase2)

# 需要 local reader 的测试：core + local
add_executable(test_vipdoc_reader test_vipdoc_reader.cpp)
target_include_directories(test_vipdoc_reader PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_vipdoc_reader PRIVATE tdx_proto_local tdx_proto_core gtest gtest_main)
add_test(NAME test_vipdoc_reader COMMAND test_vipdoc_reader)

add_executable(test_golden test_golden.cpp)
target_include_directories(test_golden PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_golden PRIVATE tdx_proto_local tdx_proto_parsers tdx_proto_core gtest gtest_main)
add_test(NAME test_golden COMMAND test_golden)

# 数据层测试
tdx_test_data(test_phase3)
# test_sync4 用 custom main（MainInitGuard，SyncState fb2::Mutex 需 helio init）
add_executable(test_sync4 test_sync4.cpp)
target_include_directories(test_sync4 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_sync4 PRIVATE tdx_data base fibers2 gtest)
add_test(NAME test_sync4 COMMAND test_sync4)

# import 标的过滤（IsAStock / IsKronosTarget）：纯头文件、离线
add_executable(test_import_filter test_import_filter.cpp)
target_include_directories(test_import_filter PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_import_filter PRIVATE tdx_taos gtest gtest_main)
add_test(NAME test_import_filter COMMAND test_import_filter)
# 需要 transport 的测试：core + transport + fibers2（含 ProactorPool 编排）
add_executable(test_retry test_retry.cpp)
target_include_directories(test_retry PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_retry PRIVATE tdx_proto_transport tdx_proto_core base fibers2 gtest)
add_test(NAME test_retry COMMAND test_retry)

add_executable(test_server_pool test_server_pool.cpp)
target_include_directories(test_server_pool PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_server_pool PRIVATE tdx_proto_transport tdx_proto_core base fibers2 gtest)
add_test(NAME test_server_pool COMMAND test_server_pool)

# 链路恢复测试：Connection 协议异常标记断开 + 断开后重连恢复契约。
# 全本地 loopback（不依赖真网），custom main（含 SIGFPE 抖动重试 shim）。
add_executable(test_reconnect test_reconnect.cpp)
target_include_directories(test_reconnect PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_reconnect PRIVATE tdx_proto_transport tdx_proto_core base fibers2 gtest)
add_test(NAME test_reconnect COMMAND test_reconnect)

# 端到端集成测试（真网通达信服务器 7709）：custom main + ProactorPool
add_executable(test_e2e test_e2e.cpp)
target_include_directories(test_e2e PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_e2e PRIVATE tdx_quotes tdx_data base fibers2 gtest)
add_test(NAME test_e2e COMMAND test_e2e)

# SP/MAC 端到端测试（独立二进制：helio 不支持同进程先后两个 ProactorPool）
add_executable(test_sp_e2e test_sp_e2e.cpp)
target_include_directories(test_sp_e2e PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_sp_e2e PRIVATE tdx_quotes base fibers2 gtest)
add_test(NAME test_sp_e2e COMMAND test_sp_e2e)

# golden 测试需 fixtures 路径
target_compile_definitions(test_golden PRIVATE
    FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/golden")
# Phase 3 测试需 cfg 路径（holidays.json）
target_compile_definitions(test_phase3 PRIVATE
    CFG_DIR="${CMAKE_SOURCE_DIR}/cfg")

# Phase 6 共享内存测试（纯标准库 + 线程/fork，不需 helio）
add_executable(test_shm test_shm.cpp)
target_include_directories(test_shm PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_shm PRIVATE tdx_shm gtest gtest_main)
add_test(NAME test_shm COMMAND test_shm)
