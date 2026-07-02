// 端到端集成测试：真网连接通达信服务器（端口 7709）→ 登录 → 取数据。
// 仅在通达信服务器可达时执行（不可达时 GTEST_SKIP，不视为失败）。
// custom main（MainInitGuard）：需 helio ProactorPool fiber 编排。
//
// 覆盖链路：TCP Connect → Login(0x0d) → Heartbeat(0x04)
//           → StockCount(0x44e) → StockList(0x44d)
//           → Kline(0x523) → Quotes(0x53e) → Transaction(0xfc5)
//           → Finance(0x10) → F10(0x2cf) → HistoryOrders(0xfb4) → HistoryTx(0xfb5)
//           → VolProfile(0x51a) → IndexInfo(0x51d) → Unusual(0x563)
//           → SyncState 批次记录
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
    if (g_sq) g_sq->Close();  // Close() 内部 pool_->Stop() 已 pthread_join
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

// ==================== F10 内容 0x2d0（链式：先查目录→取第一项内容） ====================
// filename 实测为 ASCII（如 600000.txt），UTF8 直送与上游 opentdx 一致且正确；
// 此前「GBK 编码往返致内容空」的顾虑经真网验证不成立。cats[0]="最新提示" len≈20KB < u16
// 上限，单次 GetF10Content 即可拿全（fetch_quotes 的大分类分页拉取见 FetchF10FullText）。
TEST_F(E2ETest, F10Content600000) {
  auto cats = g_sq->GetF10Category(Market::SH, "600000");
  ASSERT_GE(cats.size(), 1u) << "F10 目录非空";
  auto content = g_sq->GetF10Content(Market::SH, "600000",
                                     cats[0].filename, cats[0].start, cats[0].length);
  EXPECT_FALSE(content.code.empty());
  EXPECT_FALSE(content.content.empty()) << "F10 正文非空";
}

// ==================== 请求上限保护（K 线 800 条） ====================
TEST_F(E2ETest, KlineMax800Count) {
  auto bars = g_sq->Bars(Market::SH, "600000", Period::DAILY, 0, 800);
  EXPECT_LE(bars.size(), 800u) << "返回数不应超过请求上限";
}

// ==================== 财务 0x10（600000 浦发银行） ====================
TEST_F(E2ETest, Finance600000) {
  auto f = g_sq->GetFinance(Market::SH, "600000");
  EXPECT_GT(f.liutongguben, 0.0) << "流通股本应 > 0";
  EXPECT_GT(f.zongguben, 0.0) << "总股本应 > 0";
  EXPECT_GT(f.ipo_date, 19900101u) << "上市日期应有效";
  EXPECT_FALSE(f.code.empty());
}

// ==================== F10 0x2cf（600000 分类目录） ====================
TEST_F(E2ETest, F10Category600000) {
  auto cats = g_sq->GetF10Category(Market::SH, "600000");
  ASSERT_GE(cats.size(), 1u) << "F10 目录应至少有 1 项";
  for (const auto& c : cats) {
    EXPECT_FALSE(c.name.empty()) << "分类名不应为空";
    EXPECT_FALSE(c.filename.empty()) << "文件名不应为空";
  }
}

// ==================== 成交量分布 0x51a（600000） ====================
TEST_F(E2ETest, VolProfile600000) {
  auto vp = g_sq->GetVolumeProfile(Market::SH, "600000");
  EXPECT_FALSE(vp.code.empty());
  EXPECT_GT(vp.price, 0.0) << "现价应 > 0";
  // 盘口至少有一档
  if (!vp.handicap_bid.empty()) {
    EXPECT_GT(vp.handicap_bid[0].price, 0.0);
  }
  // levels 可能为空（早盘），不强制 >0
}

// ==================== 指数信息 0x51d（399001 深证成指） ====================
TEST_F(E2ETest, IndexInfo399001) {
  auto ii = g_sq->GetIndexInfo(Market::SZ, "399001");
  EXPECT_FALSE(ii.code.empty());
  EXPECT_GT(ii.close, 0.0) << "收盘价应 > 0";
  EXPECT_GT(ii.up_count + ii.down_count, 0) << "涨跌家数应 > 0";
  // orders：price 为 get_price 原始值，可正可负。分时中个别分钟可能为空
  // （服务端间隙），故只要求绝大多数有量、非全空（验证解析未整体错位）。
  ASSERT_FALSE(ii.orders.empty());
  int nonzero = 0;
  for (const auto& o : ii.orders) if (o.vol != 0) ++nonzero;
  EXPECT_GT(nonzero * 10, static_cast<int>(ii.orders.size() * 9))
      << "应有 >90% 分钟有成交量，实际 " << nonzero << "/" << ii.orders.size();
}

// ==================== 指数信息 0x51d（999999 上证指数） ====================
TEST_F(E2ETest, IndexInfo999999) {
  auto ii = g_sq->GetIndexInfo(Market::SH, "999999");
  EXPECT_FALSE(ii.code.empty());
  EXPECT_GT(ii.close, 1000.0) << "上证指数应 > 1000";
  EXPECT_GT(ii.pre_close, 1000.0) << "昨收应 > 1000";
  EXPECT_GT(ii.high, ii.low) << "最高 > 最低";
}

// ==================== 主力异动 0x563（上海市场） ====================
TEST_F(E2ETest, UnusualSH) {
  auto items = g_sq->GetUnusual(Market::SH, 0, 20);
  // 异动数据依赖盘口活跃度，可能为 0（合法），不强制 >0
  size_t check = std::min(items.size(), size_t(3));
  for (size_t i = 0; i < check; ++i) {
    EXPECT_FALSE(items[i].code.empty());
    EXPECT_FALSE(items[i].desc.empty()) << "异动描述不应为空";
    EXPECT_GE(items[i].hour, 0);
    EXPECT_LE(items[i].hour, 23);
  }
}

// ==================== 历史委托 0xfb4（600000，固定历史日期） ====================
TEST_F(E2ETest, HistoryOrders600000) {
  // 固定最近交易日：2026-06-30
  auto orders = g_sq->GetHistoryOrders(Market::SH, "600000", 20260630);
  // 历史委托可能交易日不返回(配股除权日等)，不强制 >0
  size_t check = std::min(orders.size(), size_t(5));
  for (size_t i = 0; i < check; ++i) {
    EXPECT_GE(orders[i].price, 0.0);
  }
}

// ==================== 历史逐笔 0xfb5（600000 最近交易日） ====================
TEST_F(E2ETest, HistoryTx600000) {
  auto txns = g_sq->GetHistoryTransaction(Market::SH, "600000", 20260630, 0, 20);
  size_t check = std::min(txns.size(), size_t(5));
  for (size_t i = 0; i < check; ++i) {
    EXPECT_GE(txns[i].price, 0.0);
    EXPECT_LE(txns[i].minutes, 1440);  // 最多 24h × 60m
  }
}

// ==================== thread-affinity 回归守护（F2/F5） ====================
// P0 修复：Close 的 CancelPeriodic/socket 操作必须经 proactor_->Await（Debug DCHECK 守护）。
// 真网不可达时这些测试仍执行，守护 Close 不变量。

// 未 Connect 直接析构：proactor_==null 边界
TEST(CloseRegression, DestructWithoutConnect) {
  quotes::StdQuotes sq;
  // 析构触发 Close，proactor_==null 须跳过 Await 不崩
}

// 显式 Close + 二次 Close 安全
TEST(CloseRegression, IdempotentClose) {
  quotes::StdQuotes sq;
  sq.Close();
  EXPECT_FALSE(sq.IsConnected());
  sq.Close();  // 二次 Close 不 double-free
}

// 连接后显式 Close：守护 Await 路径（thread-affinity 关键点），独立实例避免污染 g_sq
TEST(CloseRegression, CloseAfterConnect) {
  quotes::StdQuotes sq;
  auto ec = sq.Connect();
  if (ec) GTEST_SKIP() << "服务器不可达，跳过连接后 Close 守护";
  sq.Close();
  EXPECT_FALSE(sq.IsConnected());
  sq.Close();  // 二次 Close 安全
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
