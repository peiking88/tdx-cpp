// DuckDB 查询层实现。编译 -D_GLIBCXX_USE_CXX11_ABI=1 匹配 vendored libduckdb.so v1.5.2（新 CXX11 ABI）。
// 接口 string_view，内部 std::string 转换后调 duckdb.hpp。
#include "tdx/query/duckdb_query.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <duckdb.hpp>

namespace tdx::query {
namespace {

// 转义 SQL 字符串中的单引号（标准 SQL：'' → 转义后的单引号）
std::string EscapeSql(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char ch : s) {
    if (ch == '\'') out += "''";
    else out += ch;
  }
  return out;
}

}  // namespace

struct DuckDBQuery::Impl {
  std::shared_ptr<duckdb::DuckDB> db;
  duckdb::Connection con;
  Impl() : db(std::make_shared<duckdb::DuckDB>(nullptr)), con(*db) {
    con.Query("CREATE TABLE bars(code VARCHAR, datetime BIGINT, open DOUBLE, high DOUBLE, "
              "low DOUBLE, close DOUBLE, volume DOUBLE, amount DOUBLE)");
    con.Query("CREATE TABLE latest(code VARCHAR, price DOUBLE, ts BIGINT)");
  }
  explicit Impl(const std::string& path)
      : db(std::make_shared<duckdb::DuckDB>(path)), con(*db) {}
  // 共享连接：从已有 DB 实例创建新 Connection（多线程并发安全）
  explicit Impl(std::shared_ptr<duckdb::DuckDB> shared_db) : db(shared_db), con(*db) {}
};

DuckDBQuery::DuckDBQuery() : impl_(std::make_unique<Impl>()) {}
DuckDBQuery::DuckDBQuery(std::string_view db_path)
    : impl_(std::make_unique<Impl>(std::string(db_path))) {}
DuckDBQuery::~DuckDBQuery() = default;

std::unique_ptr<DuckDBQuery> DuckDBQuery::OpenSharedConnection() const {
  // 从当前 DB 实例创建新 Connection（引用计数保护 DB 生命周期）
  return std::unique_ptr<DuckDBQuery>(
      new DuckDBQuery(std::make_unique<Impl>(impl_->db)));
}

// 私有构造：从 Impl unique_ptr 构造（绕过公开构造）
DuckDBQuery::DuckDBQuery(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}

void DuckDBQuery::WriteKlineParquet(std::string_view path, const std::vector<KLine>& bars,
                                    std::string_view code) {
  std::string p(path), c(code);
  impl_->con.Query("DELETE FROM bars");
  for (const auto& b : bars) {
    char sql[512];
    std::snprintf(sql, sizeof(sql),
                  "INSERT INTO bars VALUES ('%s', %lld, %f, %f, %f, %f, %f, %f)",
                  EscapeSql(c).c_str(), static_cast<long long>(b.datetime),
                  b.open, b.high, b.low, b.close, b.volume, b.amount);
    impl_->con.Query(sql);
  }
  impl_->con.Query("COPY bars TO '" + p + "' (FORMAT PARQUET)");
}

std::vector<KLine> DuckDBQuery::ReadKlineParquet(std::string_view path) {
  std::vector<KLine> bars;
  std::string p(path);
  auto result = impl_->con.Query(
      "SELECT datetime, open, high, low, close, volume, amount FROM '" + p + "' ORDER BY datetime");
  if (!result || result->HasError()) return bars;
  for (size_t i = 0; i < result->RowCount(); ++i) {
    KLine b;
    b.datetime = result->GetValue<int64_t>(0, i);
    b.open = result->GetValue<double>(1, i);
    b.high = result->GetValue<double>(2, i);
    b.low = result->GetValue<double>(3, i);
    b.close = result->GetValue<double>(4, i);
    b.volume = result->GetValue<double>(5, i);
    b.amount = result->GetValue<double>(6, i);
    bars.push_back(b);
  }
  return bars;
}

void DuckDBQuery::SetLatestQuote(std::string_view code, double price, int64_t ts) {
  std::string c(code);
  impl_->con.Query("DELETE FROM latest WHERE code = '" + EscapeSql(c) + "'");
  char sql[256];
  std::snprintf(sql, sizeof(sql), "INSERT INTO latest VALUES ('%s', %f, %lld)",
                EscapeSql(c).c_str(), price, static_cast<long long>(ts));
  impl_->con.Query(sql);
}

double DuckDBQuery::GetLatestQuote(std::string_view code) {
  std::string c(code);
  auto result = impl_->con.Query("SELECT price FROM latest WHERE code = '" + EscapeSql(c) + "'");
  if (!result || result->HasError() || result->RowCount() == 0) return 0.0;
  return result->GetValue<double>(0, 0);
}

int64_t DuckDBQuery::Exec(std::string_view sql) {
  std::string s(sql);
  auto result = impl_->con.Query(s);
  if (!result || result->HasError()) return -1;
  return static_cast<int64_t>(result->RowCount());
}

void DuckDBQuery::ConfigureForImport() {
  // DuckDB 原生 SET（非 SQLite PRAGMA——PRAGMA journal_mode/synchronous 等是 SQLite 语法，DuckDB 忽略）
  impl_->con.Query("SET threads=8");
  impl_->con.Query("SET memory_limit='4GB'");
  impl_->con.Query("SET enable_progress_bar=false");
}

void DuckDBQuery::EnsureKlineTable(std::string_view table_name) {
  std::string t(table_name);
  std::string sql = "CREATE TABLE IF NOT EXISTS " + t +
                    " (code VARCHAR, datetime BIGINT, open DOUBLE, high DOUBLE, "
                    "low DOUBLE, close DOUBLE, volume DOUBLE, amount DOUBLE)";
  impl_->con.Query(sql);
  // 唯一索引：防重复 + 加速增量查询
  impl_->con.Query("CREATE UNIQUE INDEX IF NOT EXISTS " + t +
                   "_idx ON " + t + " (code, datetime)");
}

int64_t DuckDBQuery::InsertKlines(std::string_view table,
                                   const std::vector<KLine>& bars,
                                   std::string_view code, bool replace) {
  if (bars.empty()) return 0;
  std::string t(table), c(code), ec = EscapeSql(c);
  if (replace) {
    impl_->con.Query("DELETE FROM " + t + " WHERE code = '" + ec + "'");
  }
  int64_t total = 0;
  // INSERT OR REPLACE 批量 SQL（Appender 只做纯 INSERT 不支持 ON CONFLICT）。
  // 事务内批量写入，fsync 由调用方 BEGIN/COMMIT 控制。
  for (size_t i = 0; i < bars.size(); i += 1000) {
    std::string sql = "INSERT OR REPLACE INTO " + t + " VALUES ";
    size_t end = std::min(i + 1000, bars.size());
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql += ", ";
      char row[384];
      std::snprintf(row, sizeof(row),
                    "('%s', %lld, %.4f, %.4f, %.4f, %.4f, %.2f, %.2f)",
                    ec.c_str(), static_cast<long long>(bars[j].datetime),
                    bars[j].open, bars[j].high, bars[j].low, bars[j].close,
                    bars[j].volume, bars[j].amount);
      sql += row;
    }
    auto result = impl_->con.Query(sql);
    if (result && !result->HasError()) total += static_cast<int64_t>(end - i);
  }
  return total;
}

int64_t DuckDBQuery::LastDatetime(std::string_view table, std::string_view code) {
  std::string t(table), c(code);
  auto result = impl_->con.Query(
      "SELECT MAX(datetime) FROM " + t + " WHERE code = '" + EscapeSql(c) + "'");
  if (!result || result->HasError() || result->RowCount() == 0) return 0;
  auto val = result->GetValue(0, 0);  // non-template → Value
  if (val.IsNull()) return 0;
  return val.GetValue<int64_t>();
}

std::vector<KLine> DuckDBQuery::ReadKlines(std::string_view table,
                                            std::string_view code) {
  std::vector<KLine> bars;
  std::string t(table), c(code);
  auto result = impl_->con.Query(
      "SELECT datetime, open, high, low, close, volume, amount FROM " + t +
      " WHERE code = '" + EscapeSql(c) + "' ORDER BY datetime");
  if (!result || result->HasError()) return bars;
  for (size_t i = 0; i < result->RowCount(); ++i) {
    KLine b;
    b.datetime = result->GetValue<int64_t>(0, i);
    b.open = result->GetValue<double>(1, i);
    b.high = result->GetValue<double>(2, i);
    b.low = result->GetValue<double>(3, i);
    b.close = result->GetValue<double>(4, i);
    b.volume = result->GetValue<double>(5, i);
    b.amount = result->GetValue<double>(6, i);
    bars.push_back(b);
  }
  return bars;
}

std::unordered_map<std::string, int64_t> DuckDBQuery::LoadImportState() {
  std::unordered_map<std::string, int64_t> out;
  auto result = impl_->con.Query(
      "SELECT code, period, last_datetime FROM import_state");
  if (!result || result->HasError()) return out;
  for (size_t i = 0; i < result->RowCount(); ++i) {
    // 用 non-template GetValue 取 duckdb::Value 再 ToString（QueryResult::GetValue<string>
    // 模板特化有缺陷，内部强转 int64_t，编译失败）
    std::string code = result->GetValue(0, i).ToString();
    std::string period = result->GetValue(1, i).ToString();
    int64_t dt = result->GetValue<int64_t>(2, i);
    out[code + "|" + period] = dt;
  }
  return out;
}

std::unordered_set<std::string> DuckDBQuery::LoadAdjustCodes() {
  std::unordered_set<std::string> out;
  auto result = impl_->con.Query("SELECT DISTINCT code FROM adjust");
  if (!result || result->HasError()) return out;
  for (size_t i = 0; i < result->RowCount(); ++i)
    out.insert(result->GetValue(0, i).ToString());
  return out;
}

}  // namespace tdx::query
