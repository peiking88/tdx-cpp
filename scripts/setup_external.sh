#!/bin/bash
# 初始化 external/ 目录（第三方依赖统一收纳，不入 git）。
# helio：symlink 到 ~/framework/dragonfly/helio（或 HELIO_SRC 环境变量）
# duckdb：vendored libduckdb（镜像下载 v1.1.3）
set -e
cd "$(dirname "$0")/.."
mkdir -p external

# helio
HELIO_SRC="${HELIO_SRC:-$HOME/framework/dragonfly/helio}"
if [ ! -e external/helio ]; then
  ln -s "$HELIO_SRC" external/helio
  echo "external/helio → $HELIO_SRC"
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

echo "external/ 初始化完成："
ls -la external/
