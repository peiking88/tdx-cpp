// 缠论核心算法测试（remove_include / check_fx / check_fxs / check_bi / CZSC）
#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "czsc/analyze/algorithms.hpp"
#include "czsc/analyze/czsc.hpp"

namespace {

czsc::RawBar MakeRawBar(int id, int64_t dt, double o, double c, double h, double l) {
  czsc::RawBar bar;
  bar.symbol = "test";
  bar.id = id;
  bar.dt = dt;
  bar.open = o;
  bar.close = c;
  bar.high = h;
  bar.low = l;
  bar.vol = 100.0;
  bar.amount = 1000.0;
  return bar;
}

czsc::NewBar MakeNewBar(int id, int64_t dt, double o, double c, double h, double l) {
  auto raw = MakeRawBar(id, dt, o, c, h, l);
  auto nb = czsc::NewBar::FromRaw(raw);
  return nb;
}

}  // namespace

// ============================================================
// remove_include 测试（utils.rs:32-103）
// ============================================================
TEST(RemoveIncludeTest, UpInclusion) {
  // k3 被 k2 向上包含
  auto k1 = MakeNewBar(1, 100, 10.0, 11.0, 11.5, 9.5);
  auto k2 = MakeNewBar(2, 101, 11.0, 11.5, 12.0, 10.5);
  // k3 完全在 k2 内部：high=11.5 <= k2.high=12.0, low=10.8 >= k2.low=10.5
  auto k3 = MakeRawBar(3, 102, 11.2, 11.3, 11.5, 10.8);

  auto [has, k4] = czsc::analyze::remove_include(k1, k2, k3);
  EXPECT_TRUE(has);  // k1.high < k2.high → Up，k2含k3 → 包含
  // Up: high=max(12.0, 11.5)=12.0, low=max(10.5, 10.8)=10.8
  EXPECT_DOUBLE_EQ(k4.high, 12.0);
  EXPECT_DOUBLE_EQ(k4.low, 10.8);
}

TEST(RemoveIncludeTest, DownInclusion) {
  // k3 被 k2 向下包含
  auto k1 = MakeNewBar(1, 100, 12.0, 11.0, 12.5, 10.5);  // k1.high > k2.high → Down
  auto k2 = MakeNewBar(2, 101, 11.0, 10.5, 11.5, 10.0);
  auto k3 = MakeRawBar(3, 102, 10.5, 10.8, 11.2, 10.3);

  auto [has, k4] = czsc::analyze::remove_include(k1, k2, k3);
  EXPECT_TRUE(has);
  // Down: high=min(11.5, 11.2)=11.2, low=min(10.0, 10.3)=10.0
  EXPECT_DOUBLE_EQ(k4.high, 11.2);
  EXPECT_DOUBLE_EQ(k4.low, 10.0);
}

TEST(RemoveIncludeTest, NoInclusion) {
  auto k1 = MakeNewBar(1, 100, 10.0, 11.0, 11.5, 9.5);
  auto k2 = MakeNewBar(2, 101, 11.0, 11.5, 12.0, 10.5);
  // k3 does not contain k2
  auto k3 = MakeRawBar(3, 102, 11.6, 12.0, 12.5, 11.2);

  auto [has, k4] = czsc::analyze::remove_include(k1, k2, k3);
  EXPECT_FALSE(has);
  EXPECT_EQ(k4.id, 3);  // k4 should be from k3 (NewBar::FromRaw)
}

TEST(RemoveIncludeTest, SameHigh) {
  // k1.high == k2.high → 直接返回 k3
  auto k1 = MakeNewBar(1, 100, 10.0, 11.0, 12.0, 9.5);
  auto k2 = MakeNewBar(2, 101, 11.0, 11.5, 12.0, 10.5);
  auto k3 = MakeRawBar(3, 102, 10.0, 10.5, 11.0, 9.0);

  auto [has, k4] = czsc::analyze::remove_include(k1, k2, k3);
  EXPECT_FALSE(has);
}

// ============================================================
// check_fx 测试（utils.rs:158-192）
// ============================================================
TEST(CheckFxTest, TopFx) {
  auto k1 = MakeNewBar(1, 100, 10.0, 11.0, 11.5, 9.5);
  auto k2 = MakeNewBar(2, 101, 11.0, 11.5, 12.0, 10.5);  // 顶分型
  auto k3 = MakeNewBar(3, 102, 11.5, 10.5, 11.8, 10.0);

  auto fx = czsc::analyze::check_fx(k1, k2, k3);
  ASSERT_TRUE(fx.has_value());
  EXPECT_EQ(fx->mark, czsc::Mark::kG);
  EXPECT_DOUBLE_EQ(fx->fx, 12.0);
  EXPECT_EQ(fx->elements.size(), 3u);
}

TEST(CheckFxTest, BottomFx) {
  auto k1 = MakeNewBar(1, 100, 12.0, 11.0, 12.5, 10.5);
  auto k2 = MakeNewBar(2, 101, 11.0, 10.5, 11.5, 10.0);  // 底分型：最低
  auto k3 = MakeNewBar(3, 102, 10.5, 11.0, 12.0, 10.3);  // high > k2.high

  auto fx = czsc::analyze::check_fx(k1, k2, k3);
  ASSERT_TRUE(fx.has_value());
  EXPECT_EQ(fx->mark, czsc::Mark::kD);
  EXPECT_DOUBLE_EQ(fx->fx, 10.0);
}

TEST(CheckFxTest, NotAFx) {
  // 连续上升，不分型
  auto k1 = MakeNewBar(1, 100, 10.0, 11.0, 11.5, 9.5);
  auto k2 = MakeNewBar(2, 101, 11.0, 11.5, 12.0, 10.5);
  auto k3 = MakeNewBar(3, 102, 11.5, 12.5, 13.0, 11.0);

  auto fx = czsc::analyze::check_fx(k1, k2, k3);
  EXPECT_FALSE(fx.has_value());
}

// ============================================================
// check_fxs 测试
// ============================================================
TEST(CheckFxsTest, MultipleFxs) {
  // 构建 V 字形：顶分型 + 底分型
  std::vector<czsc::NewBar> bars;
  bars.push_back(MakeNewBar(1, 100, 10.0, 11.0, 11.5, 9.5));
  bars.push_back(MakeNewBar(2, 101, 11.0, 11.5, 12.0, 10.5));  // 顶
  bars.push_back(MakeNewBar(3, 102, 11.5, 10.5, 11.8, 10.0));
  bars.push_back(MakeNewBar(4, 103, 10.5, 10.0, 11.0, 9.5));
  bars.push_back(MakeNewBar(5, 104, 10.0, 10.5, 10.5, 9.0));   // 底
  bars.push_back(MakeNewBar(6, 105, 10.5, 11.5, 12.0, 10.0));

  auto fxs = czsc::analyze::check_fxs(bars);
  EXPECT_GE(fxs.size(), 1u);
  // First fx (at index 2 = bar[1]) should be G
  EXPECT_EQ(fxs[0].mark, czsc::Mark::kG);
}

// ============================================================
// CZSC 测试（真实数据 002515.SZ 日线）
// ============================================================
namespace {

// 002515.SZ 日线 2025-01-02 ~ 2025-02-28，37 根
// 来源：czsc-core/src/analyze/mod.rs:757-797
std::vector<czsc::RawBar> MakeRealBars() {
  struct Row { const char* dt; double o, c, h, l, v, a; };
  static const Row rows[] = {
    {"2025-01-02", 50.73, 51.29, 52.97, 50.62, 32900684.0, 152798823.0},
    {"2025-01-03", 51.40, 48.72, 51.85, 48.60, 33224687.0, 147184323.0},
    {"2025-01-06", 48.83, 48.60, 49.39, 47.48, 17419634.0, 75608391.0},
    {"2025-01-07", 48.60, 48.94, 49.05, 48.27, 13929982.0, 60500438.0},
    {"2025-01-08", 48.27, 48.04, 48.94, 47.26, 17697397.0, 75973887.0},
    {"2025-01-09", 48.27, 48.16, 48.83, 47.60, 14284260.0, 61391856.0},
    {"2025-01-10", 48.04, 46.92, 48.94, 46.81, 16080374.0, 68834125.0},
    {"2025-01-13", 46.59, 46.92, 47.26, 45.47, 12508818.0, 52037636.0},
    {"2025-01-14", 46.92, 48.16, 48.27, 46.92, 16407679.0, 69944802.0},
    {"2025-01-15", 49.50, 49.50, 50.73, 49.05, 29140842.0, 129502353.0},
    {"2025-01-16", 49.50, 49.72, 50.28, 48.94, 19124511.0, 84774186.0},
    {"2025-01-17", 49.28, 50.28, 51.74, 49.05, 22228511.0, 99754272.0},
    {"2025-01-20", 50.40, 50.40, 50.73, 49.61, 14908933.0, 66989586.0},
    {"2025-01-21", 50.62, 50.06, 50.73, 49.61, 11565100.0, 51612511.0},
    {"2025-01-22", 50.06, 49.16, 50.06, 48.83, 10889797.0, 47963340.0},
    {"2025-01-23", 49.39, 48.72, 49.95, 48.72, 13050206.0, 57522568.0},
    {"2025-01-24", 48.49, 48.83, 48.94, 48.27, 12042388.0, 52334558.0},
    {"2025-01-27", 49.05, 49.39, 51.74, 49.05, 22813802.0, 102357601.0},
    {"2025-02-05", 49.39, 49.16, 49.95, 48.72, 13525075.0, 59524887.0},
    {"2025-02-06", 48.83, 49.05, 49.28, 48.16, 17429613.0, 75782611.0},
    {"2025-02-07", 48.94, 49.50, 49.95, 48.72, 17447114.0, 76989329.0},
    {"2025-02-10", 49.39, 50.40, 50.51, 49.16, 18733821.0, 83810683.0},
    {"2025-02-11", 50.40, 49.84, 50.73, 49.61, 13189816.0, 58803966.0},
    {"2025-02-12", 50.06, 50.06, 50.40, 49.50, 15881392.0, 70692291.0},
    {"2025-02-13", 49.84, 49.84, 50.51, 49.61, 18048669.0, 80671035.0},
    {"2025-02-14", 49.72, 49.05, 49.95, 48.94, 17455299.0, 76786904.0},
    {"2025-02-17", 49.16, 49.39, 49.61, 48.60, 15791678.0, 69303481.0},
    {"2025-02-18", 49.16, 47.71, 49.39, 47.48, 20599809.0, 88885983.0},
    {"2025-02-19", 47.48, 48.04, 48.16, 47.37, 12911258.0, 55064600.0},
    {"2025-02-20", 48.04, 48.27, 48.83, 47.71, 12823411.0, 55267260.0},
    {"2025-02-21", 48.27, 47.60, 48.72, 47.48, 16547084.0, 70527761.0},
    {"2025-02-24", 47.71, 52.41, 52.41, 47.71, 93355060.0, 426873493.0},
    {"2025-02-25", 51.96, 50.51, 51.96, 50.17, 54431026.0, 246916111.0},
    {"2025-02-26", 50.62, 52.52, 52.86, 50.17, 50584995.0, 232883144.0},
    {"2025-02-27", 52.41, 53.64, 53.98, 51.96, 47142936.0, 224200231.0},
    {"2025-02-28", 53.20, 52.52, 53.53, 52.41, 29058781.0, 137329596.0},
  };

  constexpr int N = sizeof(rows) / sizeof(rows[0]);
  std::vector<czsc::RawBar> bars(N);
  for (int i = 0; i < N; ++i) {
    bars[i].symbol = "002515.SZ";
    bars[i].id = i;
    bars[i].dt = 1735689600 + i * 86400;  // rough epoch, sufficient for monotonicity
    bars[i].freq = czsc::Freq::kDay;
    bars[i].open = rows[i].o;
    bars[i].close = rows[i].c;
    bars[i].high = rows[i].h;
    bars[i].low = rows[i].l;
    bars[i].vol = rows[i].v;
    bars[i].amount = rows[i].a;
  }
  return bars;
}

}  // namespace

// 对齐 Rust mod.rs:851-900 test_czsc_bi_list
TEST(CZSCAnalyzeTest, BiListFromRealData) {
  auto bars = MakeRealBars();
  czsc::analyze::CZSC czsc(bars, 50, 6);

  // Rust 期望 4 笔
  ASSERT_EQ(czsc.bi_list.size(), 4u);

  // 笔 0：向上，high=51.74, low=45.47
  // BI[0]: sdt=2025-01-13, edt=2025-01-17, Up, high=51.74, low=45.47
  EXPECT_EQ(czsc.bi_list[0].direction, czsc::Direction::kUp);
  EXPECT_NEAR(czsc.bi_list[0].get_high(), 51.74, 1e-4);
  EXPECT_NEAR(czsc.bi_list[0].get_low(), 45.47, 1e-4);

  // 笔 1：向下，high=51.74, low=48.16
  EXPECT_EQ(czsc.bi_list[1].direction, czsc::Direction::kDown);
  EXPECT_NEAR(czsc.bi_list[1].get_high(), 51.74, 1e-4);
  EXPECT_NEAR(czsc.bi_list[1].get_low(), 48.16, 1e-4);

  // 笔 2：向上，high=50.73, low=48.16
  EXPECT_EQ(czsc.bi_list[2].direction, czsc::Direction::kUp);
  EXPECT_NEAR(czsc.bi_list[2].get_high(), 50.73, 1e-4);
  EXPECT_NEAR(czsc.bi_list[2].get_low(), 48.16, 1e-4);

  // 笔 3：向下，high=50.73, low=47.37
  EXPECT_EQ(czsc.bi_list[3].direction, czsc::Direction::kDown);
  EXPECT_NEAR(czsc.bi_list[3].get_high(), 50.73, 1e-4);
  EXPECT_NEAR(czsc.bi_list[3].get_low(), 47.37, 1e-4);
}

// 对齐 Rust mod.rs:932-958 test_czsc_fx_list
TEST(CZSCAnalyzeTest, FxListFromRealData) {
  auto bars = MakeRealBars();
  czsc::analyze::CZSC czsc(bars, 50, 6);

  auto fxs = czsc.get_fx_list();
  // Rust 期望 12 分型
  ASSERT_EQ(fxs.size(), 12u);

  // 分型 fx 值（按 Rust 期望顺序）
  double expected_fx[] = {
    50.73, 48.94, 51.74, 48.27, 51.74, 48.16,
    50.73, 49.50, 50.51, 47.37, 48.83, 47.48
  };

  for (size_t i = 0; i < 12; ++i) {
    EXPECT_NEAR(fxs[i].fx, expected_fx[i], 1e-4)
        << "FX[" << i << "] fx mismatch";
  }
}

TEST(CZSCAnalyzeTest, EmptyBars) {
  czsc::analyze::CZSC czsc;
  EXPECT_TRUE(czsc.bi_list.empty());
  EXPECT_TRUE(czsc.bars_raw.empty());
  EXPECT_TRUE(czsc.bars_ubi.empty());
}

TEST(CZSCAnalyzeTest, SingleBarUpdate) {
  czsc::analyze::CZSC czsc;
  auto bar = MakeRawBar(0, 1000, 10.0, 11.0, 11.5, 9.5);
  czsc.update_bar(bar);

  EXPECT_EQ(czsc.bars_raw.size(), 1u);
  EXPECT_EQ(czsc.bars_ubi.size(), 1u);
  EXPECT_TRUE(czsc.bi_list.empty());  // < 3 bars_ubi → no bi
}

// ============================================================
// 一类买卖点：趋势背驰判定测试（check_first_buy / check_first_sell）
// 对齐 Rust utils/cxt.rs:219-335
// ============================================================

// 构造一根笔：direction + 端点高低 + 力度(通过 bars 长度与 vol 间接体现)
namespace {
czsc::BI MakeBI(czsc::Direction dir, double hi, double lo, size_t n_bars, double vol) {
  czsc::BI bi;
  bi.direction = dir;
  bi.symbol = "test";
  // get_power_price = |fx_b.fx - fx_a.fx|，取为价差幅度 hi - lo（恒正）
  if (dir == czsc::Direction::kUp) {
    bi.fx_a.fx = lo; bi.fx_b.fx = hi;   // 向上笔：起点低 → 终点高
  } else {
    bi.fx_a.fx = hi; bi.fx_b.fx = lo;   // 向下笔：起点高 → 终点低
  }
  bi.fx_a.high = hi; bi.fx_a.low = lo;
  bi.fx_b.high = hi; bi.fx_b.low = lo;
  // bars 决定 get_power_volume / get_length
  bi.bars.clear();
  for (size_t i = 0; i < n_bars; ++i) {
    auto nb = czsc::NewBar::FromRaw(MakeRawBar(static_cast<int32_t>(i), 1000 + i * 86400,
                                                lo + 0.1, lo + 0.2, hi, lo));
    nb.vol = vol; nb.amount = vol * 10;
    bi.bars.push_back(nb);
  }
  return bi;
}
}  // namespace

// 标准一买场景：5 笔、首末笔均向下、首笔最高、末笔最低，
// 末笔 power_price(4) 与 length(3) 均明显小于关键笔(b0/b2: power=10, length=8) → 背驰
TEST(FirstBsTest, FirstBuyBasic) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeBI(czsc::Direction::kDown, 100.0, 90.0, 8, 100.0));  // b0：最高, power=10
  bis.push_back(MakeBI(czsc::Direction::kUp,   97.0, 88.0, 6, 100.0));  // b1
  bis.push_back(MakeBI(czsc::Direction::kDown, 95.0, 85.0, 8, 100.0));  // b2：低点递降, power=10
  bis.push_back(MakeBI(czsc::Direction::kUp,   94.0, 83.0, 6, 100.0));  // b3
  bis.push_back(MakeBI(czsc::Direction::kDown, 84.0, 80.0, 3, 100.0));  // b4：最低, power=4, len=3 → 背离
  EXPECT_TRUE(czsc::analyze::check_first_buy(bis));
  EXPECT_FALSE(czsc::analyze::check_first_sell(bis));
}

// 一买不成立：末笔力度不背离（末笔 power/length 与关键笔相当）
TEST(FirstBsTest, FirstBuyNoDivergence) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeBI(czsc::Direction::kDown, 100.0, 90.0, 8, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   97.0, 88.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kDown, 95.0, 85.0, 8, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   94.0, 83.0, 6, 100.0));
  // 末笔 power=13(=93-80)、length=8，力度不背离 → bc_price 与 bc_length 均为 false
  bis.push_back(MakeBI(czsc::Direction::kDown, 93.0, 80.0, 8, 100.0));
  EXPECT_FALSE(czsc::analyze::check_first_buy(bis));
}

// 一买不成立：偶数笔
TEST(FirstBsTest, FirstBuyEvenCount) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeBI(czsc::Direction::kDown, 100.0, 90.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   97.0, 88.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kDown, 95.0, 85.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   94.0, 83.0, 6, 100.0));
  EXPECT_FALSE(czsc::analyze::check_first_buy(bis));
}

// 一买不成立：末笔向上
TEST(FirstBsTest, FirstBuyLastUp) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeBI(czsc::Direction::kDown, 100.0, 90.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   97.0, 88.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kDown, 95.0, 85.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   94.0, 83.0, 6, 100.0));
  bis.push_back(MakeBI(czsc::Direction::kUp,   93.0, 80.0, 6, 100.0));  // 末笔向上
  EXPECT_FALSE(czsc::analyze::check_first_buy(bis));
}

// 一卖场景：首末笔均向上、首笔最低、末笔最高、末笔力度背离
TEST(FirstBsTest, FirstSellBasic) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeBI(czsc::Direction::kUp,   20.0, 10.0, 8, 100.0));  // b0：最低, power=10
  bis.push_back(MakeBI(czsc::Direction::kDown, 22.0, 12.0, 6, 100.0));  // b1
  bis.push_back(MakeBI(czsc::Direction::kUp,   25.0, 14.0, 8, 100.0));  // b2：高点递升, power=11
  bis.push_back(MakeBI(czsc::Direction::kDown, 26.0, 16.0, 6, 100.0));  // b3
  bis.push_back(MakeBI(czsc::Direction::kUp,   27.0, 23.0, 3, 100.0));  // b4：最高, power=4, len=3 → 背离
  EXPECT_TRUE(czsc::analyze::check_first_sell(bis));
  EXPECT_FALSE(czsc::analyze::check_first_buy(bis));
}
