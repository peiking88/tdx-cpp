// Phase 3 数据层测试：Calendar + Adjust + Resampler。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tdx/data/adjust.hpp"
#include "tdx/data/calendar.hpp"
#include "tdx/data/resampler.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx;
using namespace tdx::data;

#ifndef CFG_DIR
#define CFG_DIR "cfg"
#endif

// ---------- Calendar ----------
class CalendarTest : public ::testing::Test {
 protected:
  void SetUp() override { cal.LoadHolidays(std::string(CFG_DIR) + "/holidays.json"); }
  Calendar cal;
};

TEST_F(CalendarTest, WeekendNotTrading) {
  EXPECT_FALSE(cal.IsTradingDay(2024, 1, 6));   // 周六
  EXPECT_FALSE(cal.IsTradingDay(2024, 1, 7));   // 周日
  EXPECT_TRUE(cal.IsTradingDay(2024, 1, 2));    // 周二
}

TEST_F(CalendarTest, Holiday) {
  EXPECT_FALSE(cal.IsTradingDay("2024-01-01"));  // 元旦
  EXPECT_FALSE(cal.IsTradingDay("2024-10-01"));  // 国庆
  EXPECT_TRUE(cal.IsTradingDay("2024-10-08"));   // 国庆后交易日
}

TEST_F(CalendarTest, TradingDaysRange) {
  auto days = cal.GetTradingDays("2024-01-01", "2024-01-05");
  // 1/1 元旦休市；1/2-5 工作日（含周末 1/6,7 不在区间）
  EXPECT_EQ(days.size(), 4u);
}

// ---------- Adjust ----------
TEST(Adjust, PerShare) {
  EXPECT_DOUBLE_EQ(PerShare(10.0), 1.0);   // 每10股10元 → 1元/股
  EXPECT_DOUBLE_EQ(PerShare(0.5), 0.5);    // 已每股（<1）
  EXPECT_DOUBLE_EQ(PerShare(1.0), 0.1);    // 每10股1元 → 0.1（R1: >=1 判定）
}

TEST(Adjust, ComputeFactorQfq) {
  // 单个除权除息事件：每10股送1股（songzhuangu=1.0 → PerShare=0.1）
  std::vector<XdxrEvent> events = {
      {"2024-06-15", 0.0, 0.0, 1.0, 0.0, 1, "除权除息"},
  };
  std::vector<KLine> kline;
  for (int d = 10; d <= 20; ++d) {
    KLine k;
    k.datetime = util::date_to_epoch(2024, 6, d);
    k.close = 10.0;
    k.open = k.high = k.low = 10.0;
    kline.push_back(k);
  }
  auto factors = ComputeFactorFromXdxr(events, kline, AdjustType::Qfq);
  ASSERT_GE(factors.size(), 1u);
  // pre_close=10(6/14)，songzhuangu=0.1
  // numerator = 10 - 0 + 0 = 10；denominator = 10*(1+0.1) = 11
  // qfq event_factor = 10/11 ≈ 0.9091（ComputeFactor 不归一，ApplyAdjust 归一）
  EXPECT_NEAR(factors[0].factor, 10.0 / 11.0, 0.001);
}

TEST(Adjust, ApplyAdjustQfq) {
  // qfq 末尾归一：最新事件后因子=1（基准）。事件前无事件覆盖的 K 线 backward-asof 无匹配（因子默认 1）。
  std::vector<XdxrEvent> events = {{"2024-06-15", 0.0, 0.0, 1.0, 0.0, 1, "除权除息"}};
  std::vector<KLine> kline;
  for (int d = 13; d <= 17; ++d) {
    KLine k;
    k.datetime = util::date_to_epoch(2024, 6, d);
    k.open = k.high = k.low = k.close = 10.0;
    kline.push_back(k);
  }
  auto factors = ComputeFactorFromXdxr(events, kline, AdjustType::Qfq);
  ASSERT_GE(factors.size(), 1u);
  EXPECT_NEAR(factors[0].factor, 10.0 / 11.0, 0.001);  // 累计因子（未归一）
  ApplyAdjust(kline, factors, AdjustType::Qfq);
  // 验证不崩溃 + 所有 K 线价格合理（>0）
  for (const auto& k : kline) {
    EXPECT_GT(k.close, 0.0);
    EXPECT_GT(k.open, 0.0);
  }
}

// ---------- Resampler ----------
TEST(Resampler, BarEndTimeAShare5m) {
  int64_t e930 = util::cst_to_epoch(2024, 6, 15, 9, 30);
  auto c = util::epoch_to_cst(BarEndTimeAShare(e930, 5));
  EXPECT_EQ(c.hour, 9);
  EXPECT_EQ(c.minute, 35);  // 9:30 5m → 9:35
}

TEST(Resampler, BarEndTimeAShare30m) {
  int64_t e930 = util::cst_to_epoch(2024, 6, 15, 9, 30);
  auto c = util::epoch_to_cst(BarEndTimeAShare(e930, 30));
  EXPECT_EQ(c.hour, 10);
  EXPECT_EQ(c.minute, 0);  // 9:30 30m → 10:00
}

TEST(Resampler, BarEndTimeAShareAfternoon) {
  int64_t e1305 = util::cst_to_epoch(2024, 6, 15, 13, 5);
  auto c = util::epoch_to_cst(BarEndTimeAShare(e1305, 15));
  EXPECT_EQ(c.hour, 13);
  EXPECT_EQ(c.minute, 15);  // 13:05 15m → 13:15
}

TEST(Resampler, Resample5mTo15m) {
  // 3 根 5m（CEIL 标签 9:35/9:40/9:45）→ 1 根 15m（9:30-9:45 → 标签 9:45）
  std::vector<KLine> kline;
  for (int i = 0; i < 3; ++i) {
    KLine k;
    k.datetime = util::cst_to_epoch(2024, 6, 15, 9, 35 + i * 5);
    k.open = 10.0 + i;
    k.high = 11.0 + i;
    k.low = 9.0 + i;
    k.close = 10.5 + i;
    k.volume = 100;
    k.amount = 1000;
    kline.push_back(k);
  }
  auto r = ResampleKline(kline, Freq::Min15);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].open, 10.0);    // first
  EXPECT_EQ(r[0].close, 12.5);   // last
  EXPECT_EQ(r[0].high, 13.0);    // max
  EXPECT_EQ(r[0].low, 9.0);      // min
  EXPECT_EQ(r[0].volume, 300);   // sum
}

TEST(Resampler, ResampleDailyToWeekly) {
  // 5 根日线（周一-周五）→ 1 根周线
  std::vector<KLine> kline;
  for (int d = 10; d <= 14; ++d) {  // 2024-06-10(周一)-14(周五)
    KLine k;
    k.datetime = util::date_to_epoch(2024, 6, d);
    k.open = d;
    k.high = d + 1;
    k.low = d - 1;
    k.close = d + 0.5;
    k.volume = 1000;
    k.amount = 10000;
    kline.push_back(k);
  }
  auto r = ResampleKline(kline, Freq::Weekly);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].open, 10);      // 周一 open
  EXPECT_EQ(r[0].close, 14.5);   // 周五 close
}
