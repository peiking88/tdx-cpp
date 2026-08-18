// TA 指标测试（ta-lib thin wrappers + TaCache）
#include <gtest/gtest.h>

#include <cmath>

#include "czsc/ta/ta_cache.hpp"
#include "czsc/ta/indicators.hpp"

namespace {

// 构造 RawBar 的辅助
czsc::RawBar MakeBar(int id, double o, double c, double h, double l,
                     double v = 100.0, double a = 1000.0) {
  czsc::RawBar bar;
  bar.id = id;
  bar.open = o;
  bar.close = c;
  bar.high = h;
  bar.low = l;
  bar.vol = v;
  bar.amount = a;
  return bar;
}

}  // namespace

// ============================================================
// EMA 测试
// ============================================================
TEST(TaWrapperTest, EmaBasic) {
  std::vector<double> close = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
  auto result = czsc::ta::ema(close.data(), close.size(), 5);
  EXPECT_EQ(result.size(), 10u);
  // ta-lib warmup: first 4 are NaN, index 4 is first valid
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(std::isnan(result[i]));
  EXPECT_FALSE(std::isnan(result[4]));
}

TEST(TaWrapperTest, SmaBasic) {
  std::vector<double> close = {1.0, 2.0, 3.0, 4.0, 5.0};
  auto result = czsc::ta::sma(close.data(), close.size(), 3);
  EXPECT_EQ(result.size(), 5u);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_FALSE(std::isnan(result[2]));
}

TEST(TaWrapperTest, WmaBasic) {
  std::vector<double> close = {1.0, 2.0, 3.0, 4.0, 5.0};
  auto result = czsc::ta::wma(close.data(), close.size(), 3);
  EXPECT_EQ(result.size(), 5u);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_FALSE(std::isnan(result[2]));
}

// ============================================================
// MACD 测试
// ============================================================
TEST(TaWrapperTest, MacdBasic) {
  std::vector<double> close = {
    10.1234, 14.9812, 9.3345, 11.7789, 13.5521, 12.0044, 15.3311, 14.2088,
    13.8877, 16.2355, 15.0222, 17.1188, 16.0044, 18.4455, 17.9933, 19.2244,
    18.1102, 17.8891, 20.0033, 19.7721, 21.1144, 20.8842, 22.3355, 21.6622,
    23.1188, 22.7744, 24.3355, 23.9911, 22.8877, 21.4455, 20.8844, 19.7733,
    21.1188, 22.5522, 23.8877, 22.6644, 24.1188, 25.2244, 24.0033, 26.1188
  };
  auto m = czsc::ta::macd(close.data(), close.size(), 12, 26, 9);
  EXPECT_EQ(m.dif.size(), 40u);
  // ta-lib warmup: slow period + signal period - 1 = 26 + 9 - 1 = 34
  // First 33 should be NaN, index 33 first valid
  for (int i = 0; i < 33; ++i) EXPECT_TRUE(std::isnan(m.dif[i]));
  EXPECT_FALSE(std::isnan(m.dif[33]));
}

// ============================================================
// RSI 测试
// ============================================================
TEST(TaWrapperTest, RsiBasic) {
  std::vector<double> close = {
    22763.8, 22769.4, 22671.5, 22864.0, 22941.1, 22778.9, 22763.9,
    23276.7, 23126.7, 23062.3, 23230.6, 23115.5, 22916.9, 22962.4,
    22974.4, 22974.4
  };
  auto r = czsc::ta::rsi(close.data(), close.size(), 6);
  // ta-lib RSI warmup = period = 6, first valid at index 6
  for (int i = 0; i < 6; ++i) EXPECT_TRUE(std::isnan(r[i]));
  EXPECT_FALSE(std::isnan(r[6]));
}

// ============================================================
// ATR / CCI 测试
// ============================================================
TEST(TaWrapperTest, AtrBasic) {
  std::vector<double> h = {12.0, 11.5, 12.5, 13.0, 12.8, 13.5};
  std::vector<double> l = {10.0, 10.2, 11.0, 11.5, 11.8, 12.0};
  std::vector<double> c = {11.0, 11.3, 12.0, 12.2, 12.5, 13.0};
  auto r = czsc::ta::atr(h.data(), l.data(), c.data(), h.size(), 3);
  EXPECT_EQ(r.size(), 6u);
  // ta-lib ATR warmup = period = 3, first valid at index 3
  EXPECT_FALSE(std::isnan(r[3]));
}

TEST(TaWrapperTest, CciBasic) {
  std::vector<double> h = {12.0, 11.5, 12.5, 13.0, 12.8, 13.5};
  std::vector<double> l = {10.0, 10.2, 11.0, 11.5, 11.8, 12.0};
  std::vector<double> c = {11.0, 11.3, 12.0, 12.2, 12.5, 13.0};
  auto r = czsc::ta::cci(h.data(), l.data(), c.data(), h.size(), 3);
  EXPECT_EQ(r.size(), 6u);
  // ta-lib CCI warmup = period-1 = 2, first valid at index 2
  EXPECT_FALSE(std::isnan(r[2]));
}

// ============================================================
// BOLL 测试
// ============================================================
TEST(TaWrapperTest, BollBasic) {
  std::vector<double> close(30, 20.0);
  for (size_t i = 20; i < 30; ++i) close[i] = 25.0;  // 尾部上升
  auto b = czsc::ta::boll(close.data(), close.size(), 20, 2.0);
  EXPECT_EQ(b.upper.size(), 30u);
  // warmup = period-1 = 19
  EXPECT_TRUE(std::isnan(b.upper[18]));
  EXPECT_FALSE(std::isnan(b.upper[19]));
}

// ============================================================
// KDJ 测试
// ============================================================
TEST(TaWrapperTest, StochBasic) {
  std::vector<double> h = {10.0, 11.0, 12.0, 11.0, 13.0, 14.0, 15.0, 14.0, 16.0, 17.0,
                           18.0, 17.0, 19.0, 20.0, 21.0, 20.0, 22.0, 23.0, 22.0, 24.0};
  std::vector<double> l = { 9.0, 10.0, 11.0, 10.0, 12.0, 13.0, 14.0, 13.0, 15.0, 16.0,
                           17.0, 16.0, 18.0, 19.0, 20.0, 19.0, 21.0, 22.0, 21.0, 23.0};
  std::vector<double> c = { 9.5, 10.5, 11.5, 10.8, 12.6, 13.2, 14.7, 13.5, 15.4, 16.8,
                           17.1, 16.4, 18.6, 19.4, 20.9, 19.7, 21.3, 22.7, 21.8, 23.4};
  auto s = czsc::ta::stoch(h.data(), l.data(), c.data(), h.size(), 9, 3, 3);
  EXPECT_EQ(s.k.size(), 20u);
  // ta-lib STOCH lookback = fastk-1 + slowk-1 + slowd-1 = 8+2+2 = 12
  // J = 3K - 2D should be non-NaN where K is non-NaN
  for (size_t i = 0; i < 12; ++i) EXPECT_TRUE(std::isnan(s.k[i]));
  EXPECT_FALSE(std::isnan(s.k[12]));
  EXPECT_FALSE(std::isnan(s.j[12]));
}

// ============================================================
// SAR 测试
// ============================================================
TEST(TaWrapperTest, SarBasic) {
  std::vector<double> h = {10.0, 11.0, 12.0, 11.0, 13.0, 12.5};
  std::vector<double> l = { 9.0, 10.0, 11.0, 10.0, 12.0, 11.5};
  auto r = czsc::ta::sar(h.data(), l.data(), h.size(), 0.02, 0.2);
  EXPECT_EQ(r.size(), 6u);
  // SAR has warmup of 1
  EXPECT_TRUE(std::isnan(r[0]));
  EXPECT_FALSE(std::isnan(r[1]));
}

// ============================================================
// TaCache 测试
// ============================================================
TEST(TaCacheTest, DefaultEmpty) {
  czsc::ta::TaCache cache;
  EXPECT_TRUE(cache.series.empty());
  EXPECT_TRUE(cache.macd.empty());
  EXPECT_TRUE(cache.boll.empty());
  EXPECT_TRUE(cache.kdj.empty());
  EXPECT_EQ(cache.last_len, 0u);
}

TEST(TaCacheTest, MaCacheFullInit) {
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 30; ++i) {
    auto b = MakeBar(i, 10.0 + i * 0.1, 10.0 + i * 0.1 + 0.05,
                     10.5 + i * 0.1, 9.8 + i * 0.1);
    bars.push_back(b);
  }
  czsc::ta::TaCache cache;
  czsc::ta::update_ma_cache(bars, "ema_5", "EMA", 5, cache);

  EXPECT_TRUE(cache.series.count("ema_5"));
  EXPECT_EQ(cache.series["ema_5"].size(), 30u);
  EXPECT_EQ(cache.last_len, 30u);
  EXPECT_TRUE(cache.series_ids.count("ema_5"));
}

TEST(TaCacheTest, MacdCacheFullInit) {
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 200; ++i)
    bars.push_back(MakeBar(i, 10.0 + i * 0.1, 10.0 + i * 0.1 + 0.05,
                           10.5 + i * 0.1, 9.8 + i * 0.1));
  czsc::ta::TaCache cache;
  czsc::ta::update_macd_cache(bars, "macd_12_26_9", 12, 26, 9, cache);
  EXPECT_TRUE(cache.macd.count("macd_12_26_9"));
  EXPECT_EQ(cache.macd["macd_12_26_9"].dif.size(), 200u);
}

TEST(TaCacheTest, BollCacheFullInit) {
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 50; ++i)
    bars.push_back(MakeBar(i, 20.0 + i * 0.01, 20.0 + i * 0.01 + 0.005,
                           20.5, 19.5));
  czsc::ta::TaCache cache;
  czsc::ta::update_boll_cache(bars, "boll_20_2.0", 20, 2.0, cache);
  EXPECT_TRUE(cache.boll.count("boll_20_2.0"));
}

TEST(TaCacheTest, CacheKeyHelpers) {
  EXPECT_EQ(czsc::ta::ma_cache_key("ema", 5), "ema_5");
  EXPECT_EQ(czsc::ta::macd_cache_key(12, 26, 9), "macd_12_26_9");
  EXPECT_EQ(czsc::ta::kdj_cache_key(9, 3, 3), "kdj_9_3_3");
  // boll key contains float formatting
  EXPECT_FALSE(czsc::ta::boll_cache_key(20, 2.0).empty());
}
