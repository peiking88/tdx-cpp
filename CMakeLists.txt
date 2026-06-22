# tdx-cpp：通达信（TDX）行情数据 C++ 单库
# 三层：协议层 tdx::proto / 行情接口层 tdx::quotes / 数据管理层 tdx::data（Phase 3）
# 异步 IO 采用 helio（io_uring + fiber），逐字移植 opentdx 协议层。
cmake_minimum_required(VERSION 3.16)

project(tdx-cpp VERSION 0.1.0 LANGUAGES C CXX)

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
    set(HELIO_PATH "/home/li/framework/dragonfly/helio")
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
add_subdirectory(${HELIO_PATH} ${CMAKE_BINARY_DIR}/helio)

# helio 暴露的 target（来自 agent 调研）：
#   base        —— 日志/flags/hash 等无协程底层
#   fibers2     —— fiber 协程 + FiberSocketBase + fb2::Mutex/CondVar + ProactorBase::AddPeriodic
#                  （经 PUBLIC 传递自动拿到 io/base/Boost::headers/absl/TRDP::cares）
#   io / strings_lib / gtest / gmock / gtest_main_ext

# ---- tdx-cpp 编译选项（helio 已全局加 -Wall -Wextra，这里仅补充）----
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wno-unused-parameter)
endif()

# 可执行统一输出到 build/bin（便于 CLI 调用）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- zlib（通达信协议解压必需；iconv 在 glibc 内置，无需额外 link）----
find_package(ZLIB REQUIRED)

# ---- 子目录 ----
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
