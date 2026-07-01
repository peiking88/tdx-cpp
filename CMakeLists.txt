# tdx-cpp：通达信（TDX）行情数据 C++ 单库
# 三层：协议层 tdx::proto / 行情接口层 tdx::quotes / 数据管理层 tdx::data（Phase 3）
# 异步 IO 采用 helio（io_uring + fiber），逐字移植 opentdx 协议层。
cmake_minimum_required(VERSION 3.16)

project(tdx-cpp VERSION 0.11.1 LANGUAGES C CXX)

# ---- C++ 标准（helio 要求 C++17）----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- helio 源码路径（可配置：CMake -DHELIO_PATH 或环境变量 HELIO_PATH）----
# 遵循全局规范「路径类参数可配置（支持环境变量或配置文件）」。
if(NOT HELIO_PATH)
  if(DEFINED ENV{HELIO_PATH})
    set(HELIO_PATH "$ENV{HELIO_PATH}")
  else()
    set(HELIO_PATH "${CMAKE_SOURCE_DIR}/external/helio")
  endif()
  set(HELIO_PATH "${HELIO_PATH}" CACHE PATH "helio 源码根目录" FORCE)
endif()
if(NOT EXISTS "${HELIO_PATH}/CMakeLists.txt")
  message(FATAL_ERROR
    "helio 未找到于 ${HELIO_PATH}/CMakeLists.txt\n"
    "请用 -DHELIO_PATH=<path> 或环境变量 HELIO_PATH 指定 helio 源码根目录")
endif()
message(STATUS "tdx-cpp: helio 路径 = ${HELIO_PATH}")

# 复用 helio 的 FetchContent 缓存：helio 已下载+patch 了 abseil/glog/gtest/benchmark，
# 直接复用其 _deps 源码可避免重复下载（abseil 的 gcc patch 若重新下载会丢失）。
if(EXISTS "${HELIO_PATH}/build/_deps" AND NOT FETCHCONTENT_BASE_DIR)
  set(FETCHCONTENT_BASE_DIR "${HELIO_PATH}/build/_deps" CACHE PATH
      "FetchContent 缓存目录（复用 helio 已下载+patch 的依赖）")
  message(STATUS "tdx-cpp: 复用 helio FetchContent 缓存 ${FETCHCONTENT_BASE_DIR}")
endif()

# ---- helio 集成（对齐 dragonfly/CMakeLists.txt:57-118 范式）----
# helio 无 install target，必须 add_subdirectory 内嵌。
list(PREPEND CMAKE_MODULE_PATH "${HELIO_PATH}/cmake")

# helio 选项必须在 add_subdirectory(helio) 之前 set：
#   - LEGACY_GLOG=OFF：走 absl log（现代路径，dragonfly 同款）
#   - WITH_AWS/GCP/AWS_CLOUD=OFF：不拉 aws-sdk（tdx-cpp 不需云存储）
#   - BUILD_DOCS=OFF：不生成文档
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
set(WITH_AWS OFF CACHE BOOL "" FORCE)
set(WITH_GCP OFF CACHE BOOL "" FORCE)
set(WITH_AWS_CLOUD OFF CACHE BOOL "" FORCE)
set(LEGACY_GLOG OFF CACHE BOOL "使用 absl log 而非 legacy glog")
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)

include_directories(${HELIO_PATH})

# ---- 第三方依赖预注入（在 add_subdirectory(helio) 之前，FetchContent 先声明优先）----
Include(FetchContent)
# googletest：预置 tarball 到构建目录，helio 的 file(DOWNLOAD) 检测到已存在则跳过。
set(GTEST_VERSION "1.17.0")
set(GTEST_STAGED "${CMAKE_SOURCE_DIR}/external/googletest/googletest-${GTEST_VERSION}.tar.gz")
if(EXISTS "${GTEST_STAGED}")
  file(COPY "${GTEST_STAGED}" DESTINATION "${CMAKE_BINARY_DIR}")
  message(STATUS "tdx-cpp: 使用本地 googletest → external/googletest/")
endif()

# benchmark：FetchContent 先声明（ghfast.top 镜像），覆盖 helio 内的 github URL。
if(EXISTS "${CMAKE_SOURCE_DIR}/external/benchmark/v1.9.5.tar.gz")
  FetchContent_Declare(
    benchmark
    URL file://${CMAKE_SOURCE_DIR}/external/benchmark/v1.9.5.tar.gz
  )
  message(STATUS "tdx-cpp: 使用本地 benchmark → external/benchmark/")
else()
  FetchContent_Declare(
    benchmark
    URL https://ghfast.top/https://github.com/google/benchmark/archive/v1.9.5.tar.gz
  )
endif()

# abseil-cpp：FetchContent 先声明（本地/ghfast.top 镜像），复制 helio 的 patch 命令。
if(EXISTS "${CMAKE_SOURCE_DIR}/external/abseil/abseil-cpp-20250512.1.tar.gz")
  set(_ABSL_URL "file://${CMAKE_SOURCE_DIR}/external/abseil/abseil-cpp-20250512.1.tar.gz")
else()
  set(_ABSL_URL "https://ghfast.top/https://github.com/abseil/abseil-cpp/releases/download/20250512.1/abseil-cpp-20250512.1.tar.gz")
endif()
FetchContent_Declare(
  abseil_cpp
  URL ${_ABSL_URL}
  PATCH_COMMAND patch -p1 < "${HELIO_PATH}/patches/abseil-20250512.1.patch"
  COMMAND patch -p1 < "${HELIO_PATH}/patches/abseil-gcc-undefined-sanitizer-compilation-fix.patch"
)
message(STATUS "tdx-cpp: 使用 abseil-cpp mirror/git → ${_ABSL_URL}")

add_subdirectory(${HELIO_PATH} ${CMAKE_BINARY_DIR}/helio)

# helio 的 cmake/internal.cmake 强制设 CMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}，
# 此处恢复为 build/bin/ 供 tdx-cpp 自身 target 使用（helio target 由末尾 redirect 块处理）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# gcs_demo 需要 GCP，但 tdx-cpp 用 WITH_GCP=OFF。将其排除出默认构建。
if(TARGET gcs_demo AND NOT WITH_GCP)
  set_target_properties(gcs_demo PROPERTIES EXCLUDE_FROM_ALL TRUE)
  message(STATUS "tdx-cpp: 已排除 gcs_demo（WITH_GCP=OFF）")
endif()

# ---- ExternalProject 依赖预置（构建期跳过网络下载，全部走 external/ 本地 tarball）----
# helio 的 add_third_party() 使用 ExternalProject_Add，下载目录为
# ${CMAKE_BINARY_DIR}/helio/third_party/<name>/。文件名由 URL 最后路径段决定。
# 在 configure 阶段将预下载的 tarball 复制到对应位置，ExternalProject 检测到
# 文件存在则跳过 download 步骤。
set(_EP_DL_BASE "${CMAKE_BINARY_DIR}/helio/third_party")
set(_EXT "${CMAKE_SOURCE_DIR}/external")

# gperf: gperftools-2.18.1.tar.gz
if(EXISTS "${_EXT}/gperf/gperftools-2.18.1.tar.gz")
  file(COPY "${_EXT}/gperf/gperftools-2.18.1.tar.gz" DESTINATION "${_EP_DL_BASE}/gperf")
  message(STATUS "tdx-cpp: 预置 gperf → external/gperf/")
endif()

# xxhash: v0.8.3.tar.gz
if(EXISTS "${_EXT}/xxhash/v0.8.3.tar.gz")
  file(COPY "${_EXT}/xxhash/v0.8.3.tar.gz" DESTINATION "${_EP_DL_BASE}/xxhash")
  message(STATUS "tdx-cpp: 预置 xxhash → external/xxhash/")
endif()

# uring (liburing): liburing-2.13.tar.gz
if(EXISTS "${_EXT}/uring/liburing-2.13.tar.gz")
  file(COPY "${_EXT}/uring/liburing-2.13.tar.gz" DESTINATION "${_EP_DL_BASE}/uring")
  message(STATUS "tdx-cpp: 预置 liburing → external/uring/")
endif()

# pugixml: v1.15.tar.gz
if(EXISTS "${_EXT}/pugixml/v1.15.tar.gz")
  file(COPY "${_EXT}/pugixml/v1.15.tar.gz" DESTINATION "${_EP_DL_BASE}/pugixml")
  message(STATUS "tdx-cpp: 预置 pugixml → external/pugixml/")
endif()

# c-ares: codeload URL 最后路径段为 v1.34.5
if(EXISTS "${_EXT}/cares/c-ares-1.34.5.tar.gz")
  file(COPY "${_EXT}/cares/c-ares-1.34.5.tar.gz" DESTINATION "${_EP_DL_BASE}/cares")
  # ExternalProject 期望的文件名是 v1.34.5（codeload URL 的最后一段）
  file(RENAME "${_EP_DL_BASE}/cares/c-ares-1.34.5.tar.gz" "${_EP_DL_BASE}/cares/v1.34.5")
  message(STATUS "tdx-cpp: 预置 c-ares → external/cares/")
endif()

# zstd: zstd-1.5.7.tar.zst
if(EXISTS "${_EXT}/zstd/zstd-1.5.7.tar.zst")
  file(COPY "${_EXT}/zstd/zstd-1.5.7.tar.zst" DESTINATION "${_EP_DL_BASE}/zstd")
  message(STATUS "tdx-cpp: 预置 zstd → external/zstd/")
endif()

# rapidjson + expected-lite：header-only，helio 用 ExternalProject GIT_REPOSITORY 拉取。
# 将 external/ 中的完整 checkout 复制到 build tree 并创建 stamp 文件，跳过 git clone。
foreach(_dep IN ITEMS rapidjson expected)
  set(_dep_src "${_EXT}/${_dep}")
  set(_dep_dst "${_EP_DL_BASE}/${_dep}")
  set(_dep_stamp "${_EP_DL_BASE}/src/${_dep}_project-stamp")
  if(EXISTS "${_dep_src}/.git")
    message(STATUS "tdx-cpp: 预置 ${_dep} → external/${_dep}/（本地 checkout）")
    # 仅在目标目录不存在时复制（避免每次 configure 都 copy）
    if(NOT EXISTS "${_dep_dst}/.git")
      file(COPY "${_dep_src}/" DESTINATION "${_dep_dst}")
    endif()
    # 创建 stamp 文件，使 ExternalProject 跳过 download+clone 步骤
    file(MAKE_DIRECTORY "${_dep_stamp}")
    file(WRITE "${_dep_stamp}/${_dep}_project-gitclone-lastrun.txt" "offline")
    file(WRITE "${_dep_stamp}/${_dep}_project-gitinfo.txt" "offline")
  endif()
endforeach()

# helio 暴露的 target（来自 agent 调研）：
#   base        —— 日志/flags/hash 等无协程底层
#   fibers2     —— fiber 协程 + FiberSocketBase + fb2::Mutex/CondVar + ProactorBase::AddPeriodic
#                  （经 PUBLIC 传递自动拿到 io/base/Boost::headers/absl/TRDP::cares）
#   io / strings_lib / gtest / gmock / gtest_main_ext

# ---- tdx-cpp 编译选项（helio 已全局加 -Wall -Wextra，这里仅补充）----
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wno-unused-parameter)
endif()

# ---- zlib-ng（vendored 替代系统 zlib，ZLIB_COMPAT 模式，API/ABI 100% 兼容）----
# 通达信协议响应解压必需。zlib-ng 兼容模式提供标准 zlib.h + libz.a，
# 零代码改动，消除项目最后一个 find_package 系统依赖。
set(ZLIB_COMPAT ON CACHE BOOL "zlib API 兼容模式" FORCE)
set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_SOURCE_DIR}/external/zlib-ng/zlib-ng-2.3.3.tar.gz")
  FetchContent_Declare(zlib
    URL file://${CMAKE_SOURCE_DIR}/external/zlib-ng/zlib-ng-2.3.3.tar.gz)
  message(STATUS "tdx-cpp: 使用本地 zlib-ng → external/zlib-ng/")
else()
  FetchContent_Declare(zlib
    URL https://ghfast.top/https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz)
  message(STATUS "tdx-cpp: zlib-ng 镜像下载（ghfast.top）")
endif()
FetchContent_MakeAvailable(zlib)

# ---- 子目录 ----
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

# ---- 将 helio 内置 test / server 可执行文件分拣到对应子目录 ----
# helio 的 cmake/internal.cmake 强制 set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})，
# 导致其 target 散落在 build/ 根目录。通过 target 级 RUNTIME_OUTPUT_DIRECTORY 覆盖。
# 使用硬编码列表（get_cmake_property(TARGETS) 无法获取嵌套 subdirectory 的 target）。
# file_test 内部使用相对路径（testdata/ids.txt.zst），需从 build/ 根目录运行，
# 不重定向。CMake test 属性作用域限制无法从顶层改 WORKING_DIRECTORY。
set(_helio_tests
    abseil_test accept_server_test arena_test cuckoo_map_test cxx_test
    expected_test fiber_socket_test fibers_test flit_test
    hash_test histogram_test io_buf_test io_test mpmc_bounded_queue_test
    mpsc_intrusive_queue_test pod_array_test ring_buffer_test strings_test
    string_view_sso_test tls_engine_test tls_socket_test uring_file_test)
set(_helio_servers echo_server ping_iouring_server raw_echo_server)
set(_helio_demos http_main https_client_cli)

foreach(_t IN LISTS _helio_tests)
  if(TARGET ${_t})
    set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  endif()
endforeach()
foreach(_s IN LISTS _helio_servers _helio_demos)
  if(TARGET ${_s})
    set_target_properties(${_s} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/demos)
  endif()
endforeach()

