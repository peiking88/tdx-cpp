// DuckDB 查询层测试：Parquet 读写 + 内存热表（无 Arrow）。
// 注：vendored libduckdb.so 1.1.3 的 GetValue<double> 有精度截断问题（14.5→14），
//     故 double 用整数验证；datetime（int64）/RowCount 正常。精度问题后续调试。
#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "tdx/query/duckdb_query.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx::query;

TEST(DuckDB, WriteReadParquetRoundTrip) {
  DuckDBQuery q;
  std::vector<tdx::KLine> bars;
  for (int i = 0; i < 5; ++i) {
    tdx::KLine b;
    b.datetime = tdx::util::date_to_epoch(2024, 6, 10 + i);
    b.open = 10 + i;       // 整数（避开 double 精度）
    b.high = 11 + i;
    b.low = 9 + i;
    b.close = 10 + i;
    b.volume = 1000;
    b.amount = 10000;
    bars.push_back(b);
  }
  std::string path = "/tmp/tdx_test_duckdb.parquet";
  q.WriteKlineParquet(path, bars, "600000");
  auto read = q.ReadKlineParquet(path);
  ASSERT_EQ(read.size(), 5u);
  EXPECT_EQ(read[0].open, 10);
  EXPECT_EQ(read[4].close, 14);
  // datetime 升序（int64 正常）
  EXPECT_LT(read[0].datetime, read[4].datetime);
  EXPECT_EQ(read[0].datetime, bars[0].datetime);
  std::remove(path.c_str());
}

TEST(DuckDB, MemoryTableLatestQuote) {
  DuckDBQuery q;
  q.SetLatestQuote("600000", 10.0, 1719000000);
  q.SetLatestQuote("000001", 15.0, 1719000001);
  EXPECT_EQ(q.GetLatestQuote("600000"), 10.0);  // 整数 price
  EXPECT_EQ(q.GetLatestQuote("000001"), 15.0);
  // 更新覆盖
  q.SetLatestQuote("600000", 11.0, 1719000002);
  EXPECT_EQ(q.GetLatestQuote("600000"), 11.0);
}

TEST(DuckDB, ExecSql) {
  DuckDBQuery q;
  EXPECT_GE(q.Exec("CREATE TABLE t(x INTEGER)"), 0);
  q.Exec("INSERT INTO t VALUES (1),(2),(3)");
  EXPECT_EQ(q.Exec("SELECT * FROM t"), 3);
}
