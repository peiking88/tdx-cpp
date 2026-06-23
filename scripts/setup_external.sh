#!/bin/bash
# 初始化 external/ 目录（第三方依赖统一收纳，不入 git）。
# helio：复制源码到 external/helio（排除 build/ .git，或 HELIO_SRC 环境变量）
# duckdb：vendored libduckdb（镜像下载 v1.1.3）
# googletest/benchmark/abseil：预下载 tarball 供 CMake 离线使用（ghfast.top 镜像）
set -e
cd "$(dirname "$0")/.."
mkdir -p external

# helio
HELIO_SRC="${HELIO_SRC:-$HOME/framework/dragonfly/helio}"
if [ ! -e external/helio/CMakeLists.txt ]; then
  mkdir -p external/helio
  rsync -a --exclude='build' --exclude='build/' --exclude='.git' --exclude='.github' --exclude='.devcontainer' --exclude='.vscode' --exclude='genfiles' "$HELIO_SRC/" external/helio/
  echo "external/helio 已复制自 $HELIO_SRC"
fi

# duckdb（如无 libduckdb.so 则下载）
if [ ! -f external/duckdb/libduckdb.so ]; then
  mkdir -p external/duckdb
  echo "下载 DuckDB libduckdb v1.1.3（镜像）..."
  curl -sL -o /tmp/duckdb.zip "https://ghfast.top/https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-linux-amd64.zip"
  (cd external/duckdb && unzip -oq /tmp/duckdb.zip)
  rm -f /tmp/duckdb.zip
  echo "external/duckdb 就绪"
fi

# googletest（如无 tarball 则下载，供 CMake 预置到构建目录）
GTEST_VERSION="1.17.0"
if [ ! -f external/googletest/googletest-${GTEST_VERSION}.tar.gz ]; then
  mkdir -p external/googletest
  echo "下载 googletest v${GTEST_VERSION}（镜像）..."
  curl -sL -o "external/googletest/googletest-${GTEST_VERSION}.tar.gz" \
    "https://ghfast.top/https://github.com/google/googletest/releases/download/v${GTEST_VERSION}/googletest-${GTEST_VERSION}.tar.gz"
  echo "external/googletest 就绪"
fi

# benchmark（如无 tarball 则下载，供 CMake FetchContent 本地 file:// 使用）
if [ ! -f external/benchmark/v1.9.5.tar.gz ]; then
  mkdir -p external/benchmark
  echo "下载 benchmark v1.9.5（镜像）..."
  curl -sL -o external/benchmark/v1.9.5.tar.gz \
    "https://ghfast.top/https://github.com/google/benchmark/archive/v1.9.5.tar.gz"
  echo "external/benchmark 就绪"
fi

# abseil-cpp（如无 tarball 则下载，供 CMake FetchContent 本地 file:// 使用）
if [ ! -f external/abseil/abseil-cpp-20250512.1.tar.gz ]; then
  mkdir -p external/abseil
  echo "下载 abseil-cpp 20250512.1（镜像）..."
  curl -sL -o external/abseil/abseil-cpp-20250512.1.tar.gz \
    "https://ghfast.top/https://github.com/abseil/abseil-cpp/releases/download/20250512.1/abseil-cpp-20250512.1.tar.gz"
  echo "external/abseil 就绪"
fi

# ---- ExternalProject 构建期依赖（供 CMake 预置到构建树，跳过 build 时下载）----

# gperf (gperftools)
if [ ! -f external/gperf/gperftools-2.18.1.tar.gz ]; then
  mkdir -p external/gperf
  echo "下载 gperftools 2.18.1（镜像）..."
  curl -sL -o external/gperf/gperftools-2.18.1.tar.gz \
    "https://ghfast.top/https://github.com/gperftools/gperftools/archive/gperftools-2.18.1.tar.gz"
  echo "external/gperf 就绪"
fi

# xxhash
if [ ! -f external/xxhash/v0.8.3.tar.gz ]; then
  mkdir -p external/xxhash
  echo "下载 xxhash v0.8.3（镜像）..."
  curl -sL -o external/xxhash/v0.8.3.tar.gz \
    "https://ghfast.top/https://github.com/Cyan4973/xxHash/archive/v0.8.3.tar.gz"
  echo "external/xxhash 就绪"
fi

# liburing
if [ ! -f external/uring/liburing-2.13.tar.gz ]; then
  mkdir -p external/uring
  echo "下载 liburing 2.13（镜像）..."
  curl -sL -o external/uring/liburing-2.13.tar.gz \
    "https://ghfast.top/https://github.com/axboe/liburing/archive/refs/tags/liburing-2.13.tar.gz"
  echo "external/uring 就绪"
fi

# pugixml
if [ ! -f external/pugixml/v1.15.tar.gz ]; then
  mkdir -p external/pugixml
  echo "下载 pugixml v1.15（镜像）..."
  curl -sL -o external/pugixml/v1.15.tar.gz \
    "https://ghfast.top/https://github.com/zeux/pugixml/archive/refs/tags/v1.15.tar.gz"
  echo "external/pugixml 就绪"
fi

# c-ares
if [ ! -f external/cares/c-ares-1.34.5.tar.gz ]; then
  mkdir -p external/cares
  echo "下载 c-ares v1.34.5（镜像）..."
  curl -sL -o external/cares/c-ares-1.34.5.tar.gz \
    "https://ghfast.top/https://github.com/c-ares/c-ares/archive/refs/tags/v1.34.5.tar.gz"
  echo "external/cares 就绪"
fi

# zstd
if [ ! -f external/zstd/zstd-1.5.7.tar.zst ]; then
  mkdir -p external/zstd
  echo "下载 zstd v1.5.7（镜像）..."
  curl -sL -o external/zstd/zstd-1.5.7.tar.zst \
    "https://ghfast.top/https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.zst"
  echo "external/zstd 就绪"
fi

# zlib-ng（替代系统 zlib，ZLIB_COMPAT 模式，API 100% 兼容，消除 find_package 系统依赖）
if [ ! -f external/zlib-ng/zlib-ng-2.3.3.tar.gz ]; then
  mkdir -p external/zlib-ng
  echo "下载 zlib-ng v2.3.3（镜像）..."
  curl -sL -o external/zlib-ng/zlib-ng-2.3.3.tar.gz \
    "https://ghfast.top/https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz"
  echo "external/zlib-ng 就绪"
fi

# rapidjson（header-only，供 helio ExternalProject 离线构建——完整 checkout 到 external/，cmake 时复制到 build tree）
if [ ! -d external/rapidjson/.git ]; then
  rm -rf external/rapidjson
  echo "克隆 rapidjson（ghfast.top 镜像）..."
  git clone --no-checkout \
    "https://ghfast.top/https://github.com/Tencent/rapidjson.git" external/rapidjson
  git -C external/rapidjson checkout ab1842a
  echo "external/rapidjson 就绪"
fi

# expected-lite（header-only，同上）
if [ ! -d external/expected/.git ]; then
  rm -rf external/expected
  echo "克隆 expected-lite（ghfast.top 镜像）..."
  git clone --no-checkout \
    "https://ghfast.top/https://github.com/martinmoene/expected-lite.git" external/expected
  git -C external/expected checkout f17940fabae07063cabb67abf2c8d164d3146044
  echo "external/expected 就绪"
fi

echo "external/ 初始化完成："
ls -la external/
