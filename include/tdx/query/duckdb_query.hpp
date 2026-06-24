// DuckDB 嵌入式查询层。无 Arrow 依赖——Parquet 读写/SQL/内存热表全走 DuckDB。
// 接口全用 string_view（接口简洁 + 跨 ABI 安全）。vendored libduckdb.so v1.5.2
// 用新 CXX11 ABI，tdx_query 编译 -D_GLIBCXX_USE_CXX11_ABI=1，与系统默认及消费者一致。
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tdx/types.hpp"

namespace tdx::query {

class DuckDBQuery {
 public:
  // 内存数据库（默认，向后兼容）
  DuckDBQuery();
  // 持久化文件数据库
  explicit DuckDBQuery(std::string_view db_path);
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

  // === 批量导入支持 ===
  // 配置 DuckDB 导入优化参数（SET threads/memory_limit 等）
  void ConfigureForImport();
  // 确保 K 线表存在（code/datetime/open/high/low/close/volume/amount）
  void EnsureKlineTable(std::string_view table_name);
  // 批量插入 K 线（每 500 行一个 INSERT，含单引号转义）。
  // 若 replace=true 则先 DELETE 该 code 的数据再插入。
  int64_t InsertKlines(std::string_view table, const std::vector<KLine>& bars,
                       std::string_view code, bool replace = true);
  // 查询某 code 在某表中的最后 datetime（增减量用），无数据返回 0。
  int64_t LastDatetime(std::string_view table, std::string_view code);

  // 从表读取 K 线（按 datetime 升序）。
  std::vector<KLine> ReadKlines(std::string_view table, std::string_view code);

  // 读取 import_state 全表，返回 "code|period" → last_datetime 映射（增量预载用）。
  std::unordered_map<std::string, int64_t> LoadImportState();
  // 读取 adjust 表已有的 distinct code 集合（复权增量跳过用）。
  std::unordered_set<std::string> LoadAdjustCodes();

  // 打开共享连接（新 Connection → 同一 DB）。多线程并发安全。
  std::unique_ptr<DuckDBQuery> OpenSharedConnection() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  // 私有构造：从 Impl 构造（OpenSharedConnection 专用）
  explicit DuckDBQuery(std::unique_ptr<Impl> p);
};

}  // namespace tdx::query
