// DuckDB 嵌入式查询层。无 Arrow 依赖——Parquet 读写/SQL/内存热表全走 DuckDB。
// 接口全用 string_view（跨 ABI 安全，因 vendored libduckdb.so 用旧 CXX11 ABI，
// tdx_query 编译 -D_GLIBCXX_USE_CXX11_ABI=0，与消费者新 ABI 隔离）。
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "tdx/types.hpp"

namespace tdx::query {

class DuckDBQuery {
 public:
  DuckDBQuery();
  ~DuckDBQuery();

  DuckDBQuery(const DuckDBQuery&) = delete;
  DuckDBQuery& operator=(const DuckDBQuery&) = delete;

  // KLine → Parquet（DuckDB COPY TO）
  void WriteKlineParquet(std::string_view path, const std::vector<KLine>& bars,
                         std::string_view code);
  // Parquet → KLine（DuckDB SELECT FROM）
  std::vector<KLine> ReadKlineParquet(std::string_view path);

  // 内存热表（替代 dragonfly 热缓存）
  void SetLatestQuote(std::string_view code, double price, int64_t ts);
  double GetLatestQuote(std::string_view code);

  // 即席 SQL，返回行数（-1 = 错误）
  int64_t Exec(std::string_view sql);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdx::query
