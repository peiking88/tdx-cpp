// DuckDB 查询层实现。编译 -D_GLIBCXX_USE_CXX11_ABI=0 匹配 vendored libduckdb.so。
// 接口 string_view，内部 std::string（旧 ABI）转换后调 duckdb.hpp。
#include "tdx/query/duckdb_query.hpp"

#include <cstdio>
#include <memory>
#include <string>

#include <duckdb.hpp>

namespace tdx::query {

struct DuckDBQuery::Impl {
  duckdb::DuckDB db;
  duckdb::Connection con;
  Impl() : db(nullptr), con(db) {
    con.Query("CREATE TABLE bars(code VARCHAR, datetime BIGINT, open DOUBLE, high DOUBLE, "
              "low DOUBLE, close DOUBLE, volume DOUBLE, amount DOUBLE)");
    con.Query("CREATE TABLE latest(code VARCHAR, price DOUBLE, ts BIGINT)");
  }
};

DuckDBQuery::DuckDBQuery() : impl_(std::make_unique<Impl>()) {}
DuckDBQuery::~DuckDBQuery() = default;

void DuckDBQuery::WriteKlineParquet(std::string_view path, const std::vector<KLine>& bars,
                                    std::string_view code) {
  std::string p(path), c(code);
  impl_->con.Query("DELETE FROM bars");
  for (const auto& b : bars) {
    char sql[512];
    std::snprintf(sql, sizeof(sql),
                  "INSERT INTO bars VALUES ('%s', %lld, %f, %f, %f, %f, %f, %f)",
                  c.c_str(), static_cast<long long>(b.datetime),
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
  // DuckDB 1.1.3 不支持 INSERT OR REPLACE，改 DELETE + INSERT
  impl_->con.Query("DELETE FROM latest WHERE code = '" + c + "'");
  char sql[256];
  std::snprintf(sql, sizeof(sql), "INSERT INTO latest VALUES ('%s', %f, %lld)",
                c.c_str(), price, static_cast<long long>(ts));
  impl_->con.Query(sql);
}

double DuckDBQuery::GetLatestQuote(std::string_view code) {
  std::string c(code);
  auto result = impl_->con.Query("SELECT price FROM latest WHERE code = '" + c + "'");
  if (!result || result->HasError() || result->RowCount() == 0) return 0.0;
  return result->GetValue<double>(0, 0);
}

int64_t DuckDBQuery::Exec(std::string_view sql) {
  std::string s(sql);
  auto result = impl_->con.Query(s);
  if (!result || result->HasError()) return -1;
  return static_cast<int64_t>(result->RowCount());
}

}  // namespace tdx::query
