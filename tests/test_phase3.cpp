// Phase 3 数据层测试：Calendar + Adjust + Resampler。
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <fstream>

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

// 缺失文件不崩溃，降级为「仅周末非交易日」
TEST(Calendar, MissingHolidaysFile) {
  Calendar cal("/tmp/tdx_test_no_such_holidays.json");
  EXPECT_TRUE(cal.IsTradingDay(2024, 1, 2));   // 周二（无 json 仅周末判断）
  EXPECT_FALSE(cal.IsTradingDay(2024, 1, 6));  // 周六
}

TEST(Calendar, CorruptedHolidaysFile) {
  // 写入损坏 JSON → LoadHolidays 静默降级
  {
    std::ofstream f("/tmp/tdx_test_corrupt_holidays.json");
    f << "{{{bad json";
  }
  Calendar cal("/tmp/tdx_test_corrupt_holidays.json");
  EXPECT_TRUE(cal.IsTradingDay(2024, 1, 2));  // 降级后仅周末判断
  // 元旦本应是假日，但 json 损坏 → 按非假日处理
  EXPECT_TRUE(cal.IsTradingDay(2024, 1, 1));
  std::remove("/tmp/tdx_test_corrupt_holidays.json");
}

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
  std::vector<Xdxr> events = {
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
  // qfq event_factor = 10/11 ≈ 0.9091（累乘即最终因子，不归一）
  EXPECT_NEAR(factors[0].factor, 10.0 / 11.0, 0.001);
}

TEST(Adjust, ApplyAdjustQfqSingleEvent) {
  // qfq：除权前 bar 乘事件因子（10送1 → ef=10/11），除权日及之后 factor=1。
  // 回归：曾因「末尾归一」把唯一因子除成 1.0，单除权事件股票 qfq 完全失效。
  std::vector<Xdxr> events = {{"2024-06-15", 0.0, 0.0, 1.0, 0.0, 1, "除权除息"}};
  std::vector<KLine> kline;
  for (int d = 13; d <= 17; ++d) {
    KLine k;
    k.datetime = util::date_to_epoch(2024, 6, d);
    k.open = k.high = k.low = k.close = 10.0;
    kline.push_back(k);
  }
  auto factors = ComputeFactorFromXdxr(events, kline, AdjustType::Qfq);
  ASSERT_GE(factors.size(), 1u);
  EXPECT_NEAR(factors[0].factor, 10.0 / 11.0, 0.001);  // 累乘即最终因子（不归一）
  ApplyAdjust(kline, factors, AdjustType::Qfq);
  // 6/13-14（除权前）: 10 × 10/11 ≈ 9.0909
  for (int i = 0; i <= 1; ++i) {
    EXPECT_NEAR(kline[i].open, 100.0 / 11.0, 1e-9);
    EXPECT_NEAR(kline[i].close, 100.0 / 11.0, 1e-9);
  }
  // 6/15-17（除权日及之后）: 除权日 bar 不乘自身事件，factor=1 → 10.0
  for (int i = 2; i <= 4; ++i) {
    EXPECT_DOUBLE_EQ(kline[i].open, 10.0);
    EXPECT_DOUBLE_EQ(kline[i].close, 10.0);
  }
}

TEST(Adjust, ApplyAdjustHfq) {
  // hfq（后复权）：event_factor=denominator/numerator=11/10=1.1，无归一化。
  // forward-asof：事件前 K 线乘因子=1.1(→11.0)，事件后因子默认=1.0(→10.0)。
  std::vector<Xdxr> events = {{"2024-06-15", 0.0, 0.0, 1.0, 0.0, 1, "除权除息"}};
  std::vector<KLine> kline;
  for (int d = 13; d <= 17; ++d) {
    KLine k;
    k.datetime = util::date_to_epoch(2024, 6, d);
    k.open = k.high = k.low = k.close = 10.0;
    kline.push_back(k);
  }
  auto factors = ComputeFactorFromXdxr(events, kline, AdjustType::Hfq);
  ASSERT_GE(factors.size(), 1u);
  EXPECT_NEAR(factors[0].factor, 11.0 / 10.0, 0.001);  // hfq 因子 = 1.1

  ApplyAdjust(kline, factors, AdjustType::Hfq);
  // 6/13-15(事件日及之前): 10.0 * 1.1 = 11.0
  // forward-asof: date >= kdate → 事件日当天也匹配因子
  for (int i = 0; i <= 2; ++i) {  // 6/13, 6/14, 6/15
    EXPECT_DOUBLE_EQ(kline[i].open, 11.0);
    EXPECT_DOUBLE_EQ(kline[i].close, 11.0);
  }
  // 6/16-17(事件日后): forward-asof 无匹配 → 默认因子 1.0
  for (int i = 3; i < 5; ++i) {
    EXPECT_DOUBLE_EQ(kline[i].open, 10.0);
    EXPECT_DOUBLE_EQ(kline[i].close, 10.0);
  }
}

TEST(Adjust, ApplyAdjustQfqSplitExDateNoGap) {
  // 回归①：除权日 bar 只乘其后事件因子（不含自身送转），否则前复权序列在除权日保留假跳空。
  // 回归②：不归一——旧「末尾归一」把最新事件因子约成 1.0，其除权效果丢失（9 月分红 0.1 元未除净）。
  // 事件: 2024-06-01 10送10 (ef=0.5); 2024-09-01 每10股分红1元 (fenhong=1.0→0.1/股, ef=0.998)。
  // 原始: 5月 close=100 (除权前), 6月 close=50 (送转后), 9月 close=49 (分红后)。
  // 前复权: 5月×(0.5×0.998)=49.9, 6月×0.998=49.9, 9月×1.0=49 —— 两处除权日均无假跳空
  // (9/2 校验: 49.9×(0.98/0.998)=49, 即去掉分红后的真实跌幅)。
  // 旧 backward-asof 会把 6/1 自身 0.5 也算入 → 6月 bar×0.5=25, 出现 4× 假跳空。
  std::vector<Xdxr> events = {
      {"2024-06-01", 0.0, 0.0, 10.0, 0.0, 1, "除权除息"},  // 10送10
      {"2024-09-01", 1.0, 0.0, 0.0, 0.0, 1, "除权除息"},  // 每股分红1元
  };
  std::vector<KLine> kline;
  auto add = [&](int y, int m, int d0, int d1, double close) {
    for (int d = d0; d <= d1; ++d) {
      KLine k;
      k.datetime = util::date_to_epoch(y, m, d);
      k.open = k.high = k.low = k.close = close;
      kline.push_back(k);
    }
  };
  add(2024, 5, 20, 31, 100.0);  // 除权前
  add(2024, 6, 1, 30, 50.0);    // 除权日 bar 已是送转后价格
  add(2024, 9, 2, 20, 49.0);    // 分红后
  auto factors = ComputeFactorFromXdxr(events, kline, AdjustType::Qfq);
  ApplyAdjust(kline, factors, AdjustType::Qfq);
  auto close_at = [&](int y, int m, int d) {
    int64_t t = util::date_to_epoch(y, m, d);
    for (const auto& k : kline)
      if (k.datetime == t) return k.close;
    return -1.0;
  };
  EXPECT_NEAR(close_at(2024, 5, 31), 49.9, 1e-9);  // 除权前 bar 乘全部后续事件 0.5×0.998
  EXPECT_NEAR(close_at(2024, 6, 1), 49.9, 1e-9);   // 除权日 bar 不乘自身事件，只乘其后 0.998 (旧代码会变 25)
  EXPECT_NEAR(close_at(2024, 9, 2), 49.0, 1e-9);   // 最新事件后 bar 因子=1 (归一版为 50/50/49, 9月分红未除净)
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

TEST(Resampler, BarEndTimeAShareLunchBoundary) {
  // 11:25 的 5m bar → 11:30（午休前最后一根）
  int64_t e1125 = util::cst_to_epoch(2024, 6, 15, 11, 25);
  auto c = util::epoch_to_cst(BarEndTimeAShare(e1125, 5));
  EXPECT_EQ(c.hour, 11);
  EXPECT_EQ(c.minute, 30);
}

TEST(Resampler, BarEndTimeAShareCloseBoundary) {
  // 14:55 的 5m bar → 15:00（收盘前最后一根）
  int64_t e1455 = util::cst_to_epoch(2024, 6, 15, 14, 55);
  auto c = util::epoch_to_cst(BarEndTimeAShare(e1455, 5));
  EXPECT_EQ(c.hour, 15);
  EXPECT_EQ(c.minute, 0);
}

TEST(Resampler, BarEndTimeZeroMinutes) {
  // P1 修复：period_minutes=0 防御除零，应原样返回 epoch
  int64_t e930 = util::cst_to_epoch(2024, 6, 15, 9, 30);
  EXPECT_EQ(BarEndTimeAShare(e930, 0), e930);
}

TEST(Resampler, BarEndTimeAShareAfternoon30m) {
  // 13:00 30m bar → 13:30
  int64_t e1300 = util::cst_to_epoch(2024, 6, 15, 13, 0);
  auto c = util::epoch_to_cst(BarEndTimeAShare(e1300, 30));
  EXPECT_EQ(c.hour, 13);
  EXPECT_EQ(c.minute, 30);
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
