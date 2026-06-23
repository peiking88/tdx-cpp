// 端到端集成测试：真网连接通达信服务器（端口 7709）→ 登录 → 取数据 → 落盘 DuckDB。
// 仅在通达信服务器可达时执行（不可达时 GTEST_SKIP，不视为失败）。
// custom main（MainInitGuard）：需 helio ProactorPool fiber 编排。
//
// 覆盖链路：TCP Connect → Login(0x0d) → Heartbeat(0x04)
//           → StockCount(0x44e) → StockList(0x44d)
//           → Kline(0x523) → Quotes(0x53e) → Transaction(0xfc5)
//           → DuckDB Parquet 读写 → SyncState 批次记录
//
// 验证策略：返回数量 > 50 的，仅验证样本或总数（遵循全局规范）。
//
// 注意：StdQuotes 内部管理自己的 ProactorPool + Proactor，Bars/Quotes 等方法
// 内部已调用 proactor_->Await() 封送 fiber——测试代码直接调用即可，无需外层 Await。
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <system_error>

#include "base/init.h"

#include "tdx/data/sync_state.hpp"
#include "tdx/query/duckdb_query.hpp"
#include "tdx/quotes/std_quotes.hpp"

using namespace tdx;

// ---- suite 级 fixture：一条 TCP 连接供全部测试复用 ----
class E2ETest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    g_sq = std::make_unique<quotes::StdQuotes>();
    g_ec = g_sq->Connect();  // 内部创建 ProactorPool + 选服 + 登录 + 心跳
    if (g_ec) {
      FAIL() << "初始化连接失败: " << g_ec.message();
    }
    if (!g_sq->IsConnected()) {
      FAIL() << "连接后 IsConnected() 应为 true";
    }
  }

  static void TearDownTestSuite() {
    if (g_sq) g_sq->Close();
    g_sq.reset();
  }

  static std::unique_ptr<quotes::StdQuotes> g_sq;
  static std::error_code g_ec;
};

std::unique_ptr<quotes::StdQuotes> E2ETest::g_sq;
std::error_code E2ETest::g_ec;

// ==================== 协议连通性 ====================
TEST_F(E2ETest, ConnectLoginOk) {
  EXPECT_TRUE(g_sq->IsConnected());
}

// ==================== 股票数量 0x44e ====================
TEST_F(E2ETest, StockCountSH) {
  uint16_t cnt = g_sq->StockCount(Market::SH);  // Bars 内部 Await，外层直接调
  EXPECT_GT(cnt, 2000u) << "上海市场股票数量应大于2000";
}

// ==================== 股票列表 0x44d（样本验证） ====================
TEST_F(E2ETest, StockListSample) {
  auto stocks = g_sq->Stocks(Market::SH, 0, 5);
  ASSERT_EQ(stocks.size(), 5u);
  for (const auto& s : stocks) {
    EXPECT_FALSE(s.code.empty());
    EXPECT_FALSE(s.name.empty());
    if (s.code.size() >= 1 && s.code[0] == '6') {
      EXPECT_EQ(s.market, 1) << s.code << " market 应为 SH=1";
    }
  }
}

// ==================== K 线 0x523（600000 浦发银行 日线 10 根） ====================
TEST_F(E2ETest, Kline600000Daily) {
  auto bars = g_sq->Bars(Market::SH, "600000", Period::DAILY, 0, 10);
  ASSERT_GE(bars.size(), 1u) << "应至少返回 1 根 K 线";
  for (const auto& k : bars) {
    EXPECT_GT(k.high, 0.0) << "最高价应 > 0";
    EXPECT_GE(k.high, k.low) << "最高价 ≥ 最低价";
    EXPECT_GE(k.low, 0.0) << "最低价应 >= 0";
    EXPECT_NE(k.datetime, 0) << "日期不应为 epoch 0";
    EXPECT_GT(k.volume, 0.0) << "成交量应 > 0";
    // OHLC 均在 [low, high] 区间
    EXPECT_LE(k.low, k.open);
    EXPECT_LE(k.low, k.close);
    EXPECT_GE(k.high, k.open);
    EXPECT_GE(k.high, k.close);
  }
  // 按时间升序
  for (size_t i = 1; i < bars.size(); ++i) {
    EXPECT_LT(bars[i - 1].datetime, bars[i].datetime) << "K 线时间应升序";
  }
}

// ==================== 五档报价 0x53e（600000） ====================
TEST_F(E2ETest, Quotes600000) {
  auto quotes = g_sq->Quotes({{Market::SH, "600000"}});
  ASSERT_EQ(quotes.size(), 1u);
  const auto& q = quotes[0];
  EXPECT_EQ(q.code, "600000");
  EXPECT_GT(q.price, 0.0) << "最新价应 > 0";
  EXPECT_GT(q.pre_close, 0.0) << "昨收应 > 0";
  // 买一 ≤ 卖一（正常盘口）
  if (q.bid[0] > 0 && q.ask[0] > 0) {
    EXPECT_LE(q.bid[0], q.ask[0]) << "买一 ≤ 卖一";
  }
}

// ==================== 逐笔成交 0xfc5（600000 最近 20 笔） ====================
TEST_F(E2ETest, Transaction600000) {
  auto txns = g_sq->Transactions(Market::SH, "600000", 0, 20);
  ASSERT_GE(txns.size(), 1u) << "应至少返回 1 笔逐笔成交";
  size_t check = std::min(txns.size(), size_t(3));
  for (size_t i = 0; i < check; ++i) {
    EXPECT_GE(txns[i].price, 0.0);
    EXPECT_GT(txns[i].volume, 0);
    EXPECT_TRUE(txns[i].buy_sell == BuySell::Buy ||
                txns[i].buy_sell == BuySell::Sell ||
                txns[i].buy_sell == BuySell::Neutral);
  }
}

// ==================== DuckDB Parquet 落盘 + 读回 ====================
TEST_F(E2ETest, DuckDBParquetRoundTrip) {
  auto bars = g_sq->Bars(Market::SH, "600000", Period::DAILY, 0, 20);
  ASSERT_GE(bars.size(), 1u) << "需至少 1 根 K 线才能测试 Parquet";

  query::DuckDBQuery dq;
  std::string path = "/tmp/tdx_e2e_600000.parquet";
  dq.WriteKlineParquet(path, bars, "600000");

  auto read = dq.ReadKlineParquet(path);
  ASSERT_EQ(read.size(), bars.size()) << "Parquet 读写数量应一致";
  EXPECT_EQ(read[0].datetime, bars[0].datetime);
  EXPECT_EQ(read.back().datetime, bars.back().datetime);
  std::remove(path.c_str());
}

// ==================== SyncState 断点续传记录 ====================
TEST_F(E2ETest, SyncStateBatch) {
  data::SyncState ss("/tmp/tdx_e2e_sync.json");
  ss.Clear();
  ss.StartBatch("e2e_batch_01");
  ss.MarkStockComplete("600000", "history_kline", "e2e_batch_01");
  ss.MarkStockComplete("000001", "history_kline", "e2e_batch_01");

  // 崩溃恢复
  data::SyncState ss2("/tmp/tdx_e2e_sync.json");
  ss2.Load();
  EXPECT_TRUE(ss2.IsCompletedInBatch("600000", "history_kline", "e2e_batch_01"));
  EXPECT_TRUE(ss2.IsCompletedInBatch("000001", "history_kline", "e2e_batch_01"));
  EXPECT_FALSE(ss2.IsCompletedInBatch("600001", "history_kline", "e2e_batch_01"));
  ss2.Clear();
}

// ==================== 指数 K 线（000001 上证指数） ====================
TEST_F(E2ETest, Kline000001Index) {
  auto bars = g_sq->Bars(Market::SH, "000001", Period::DAILY, 0, 10);
  ASSERT_GE(bars.size(), 1u) << "上证指数应有 K 线数据";
  for (const auto& k : bars) {
    EXPECT_GT(k.high, 0.0);
    EXPECT_GE(k.high, k.low);
  }
}

// ==================== 请求上限保护（K 线 800 条） ====================
TEST_F(E2ETest, KlineMax800Count) {
  auto bars = g_sq->Bars(Market::SH, "600000", Period::DAILY, 0, 800);
  EXPECT_LE(bars.size(), 800u) << "返回数不应超过请求上限";
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
