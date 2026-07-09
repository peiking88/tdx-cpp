// bars / ex-bars 真网测试：验证标准行情 + 扩展行情 K 线拉取。
// 仅在通达信服务器可达时执行（不可达时 GTEST_SKIP，不视为失败）。
// custom main（MainInitGuard）：StdQuotes/ExtQuotes 需 helio ProactorPool。
//
// 对齐原 CLI `tdx bars <code> <period> <count>` / `tdx ex-bars <market> <code> <period> <count>`：
//   标准行情：StdQuotes::Bars(market, code, period, 0, count)，OHLCV + amount
//   扩展行情：ExtQuotes::Bars(exmarket, code, period, 0, count)，OHLCV（无 amount）
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <system_error>

#include "base/init.h"

#include "tdx/quotes/std_quotes.hpp"
#include "tdx/quotes/ext_quotes.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx;

// ---- suite 级 fixture：一条标准行情 + 一条扩展行情连接，供全部测试复用 ----
class BarsTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    g_sq = std::make_unique<quotes::StdQuotes>();
    if (auto ec = g_sq->Connect()) {
      GTEST_SKIP() << "StdQuotes 连接失败: " << ec.message();
      return;
    }
    if (!g_sq->IsConnected()) {
      GTEST_SKIP() << "StdQuotes 连接后 IsConnected() 应为 true";
      return;
    }

    g_eq = std::make_unique<quotes::ExtQuotes>();
    if (auto ec = g_eq->Connect()) {
      GTEST_SKIP() << "ExtQuotes 连接失败: " << ec.message();
      return;
    }
    if (!g_eq->IsConnected()) {
      GTEST_SKIP() << "ExtQuotes 连接后 IsConnected() 应为 true";
      return;
    }
  }

  static void TearDownTestSuite() {
    if (g_sq) g_sq->Close();
    if (g_eq) g_eq->Close();
    g_sq.reset();
    g_eq.reset();
  }

  void SetUp() override {
    if (!g_sq || !g_sq->IsConnected() || !g_eq || !g_eq->IsConnected())
      GTEST_SKIP() << "跳过：bars 连接不可用";
  }

  static std::unique_ptr<quotes::StdQuotes> g_sq;
  static std::unique_ptr<quotes::ExtQuotes> g_eq;
};
std::unique_ptr<quotes::StdQuotes> BarsTest::g_sq;
std::unique_ptr<quotes::ExtQuotes> BarsTest::g_eq;

// ---- 标准行情 bars ----
// 对齐原 `tdx bars sh600000 4 10`（日线 10 根）。
TEST_F(BarsTest, StdDailyBarsCount) {
  constexpr int kCount = 10;
  auto bars = g_sq->Bars(Market::SH, "600000", Period::DAILY, 0, kCount);
  EXPECT_EQ(bars.size(), static_cast<size_t>(kCount));
}

// 原 bars 遍历打印 OHLCV + amount，这里验证每根字段合理（交易时段 + 正数 + high≥low）。
TEST_F(BarsTest, StdDailyBarsContent) {
  auto bars = g_sq->Bars(Market::SH, "600000", Period::DAILY, 0, 10);
  ASSERT_FALSE(bars.empty());
  for (const auto& b : bars) {
    EXPECT_GT(b.open, 0.0);
    EXPECT_GT(b.close, 0.0);
    EXPECT_GT(b.high, 0.0);
    EXPECT_GT(b.low, 0.0);
    EXPECT_GE(b.high, b.low);
    EXPECT_GE(b.volume, 0.0);
    EXPECT_GT(b.amount, 0.0);
  }
}

// 分钟线 bars（原 CLI 支持 period 8=1m）。
TEST_F(BarsTest, StdMinuteBars) {
  auto bars = g_sq->Bars(Market::SH, "600000", Period::MIN_1, 0, 5);
  // 分钟线可能因非交易时段返回空，>0 时校验时段。
  for (const auto& b : bars) {
    auto c = tdx::util::epoch_to_cst(b.datetime);
    int mins = c.hour * 60 + c.minute;
    bool am = (mins >= 570 && mins <= 690);   // 9:30-11:30
    bool pm = (mins >= 780 && mins <= 900);   // 13:00-15:00
    EXPECT_TRUE(am || pm) << "分钟 bar 时间 " << c.hour << ":" << c.minute
                          << " 应在交易时段内";
    EXPECT_GE(b.high, b.low);
  }
}

// ---- 扩展行情 ex-bars ----
// 对齐原 `tdx ex-bars 31 HSI 4 10`（港股主连日线）。
TEST_F(BarsTest, ExtDailyBarsCount) {
  constexpr int kCount = 10;
  auto bars = g_eq->Bars(ExMarket::HkMainBoard, "HSI", Period::DAILY, 0, kCount);
  // 扩展行情 host 偶有限流/不可达，返回空时跳过而非失败。
  if (bars.empty()) GTEST_SKIP() << "扩展行情 HSI 返回空（服务器限流/不可达），跳过";
  EXPECT_EQ(bars.size(), static_cast<size_t>(kCount));
}

// 原 ex-bars 遍历打印 OHLCV（无 amount），这里验证字段。
TEST_F(BarsTest, ExtDailyBarsContent) {
  auto bars = g_eq->Bars(ExMarket::HkMainBoard, "HSI", Period::DAILY, 0, 10);
  if (bars.empty()) GTEST_SKIP() << "扩展行情 HSI 返回空，跳过";
  for (const auto& b : bars) {
    EXPECT_GT(b.open, 0.0);
    EXPECT_GT(b.close, 0.0);
    EXPECT_GT(b.high, 0.0);
    EXPECT_GT(b.low, 0.0);
    EXPECT_GE(b.high, b.low);
    EXPECT_GE(b.volume, 0.0);
  }
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
