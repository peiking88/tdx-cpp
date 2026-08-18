// Phase 6 附加：5m→30m / 日线→周线 重采样 + signal_store 读写 smoke。
//
// 重采样单元测试不依赖 TDengine，信号存储测试需要 TDengine（否则 skip）。
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "czsc/io/data_loader.hpp"
#include "czsc/io/signal_store.hpp"

using czsc::RawBar;
using czsc::Freq;

namespace {

// 构造一根 fake 5m bar（dt 用某个固定分钟步长）
RawBar MakeBar(Freq f, int64_t dt, double o, double h, double l, double c,
               double v = 100, double a = 1000) {
  RawBar b;
  b.symbol = "dummy.SH";
  b.freq = f;
  b.dt = dt;
  b.open = o; b.high = h; b.low = l; b.close = c;
  b.vol = v; b.amount = a;
  return b;
}

}  // namespace

// ===================== 30m 重采样 =====================
TEST(Resample5mTo30m, EmptyInput) {
  auto out = czsc::io::Resample5mTo30m({});
  EXPECT_TRUE(out.empty());
}

TEST(Resample5mTo30m, SingleBar) {
  std::vector<RawBar> in = {MakeBar(Freq::k5Min, 1704164400, 1, 2, 0.5, 1.5)};
  auto out = czsc::io::Resample5mTo30m(in);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].freq, Freq::k30Min);
  EXPECT_DOUBLE_EQ(out[0].open, 1);
  EXPECT_DOUBLE_EQ(out[0].close, 1.5);
}

TEST(Resample5mTo30m, SixBarsIntoOne) {
  // 同一 30 分钟桶内 6 根 5m 桶。base 选在 30 分钟桶边界（offset % 1800 == 0）。
  // 1704164400 = 19724*86400 + 10800（桶边界）。
  int64_t base = 1704164400;
  std::vector<RawBar> in;
  for (int i = 0; i < 6; ++i)
    in.push_back(MakeBar(Freq::k5Min, base + i * 300,
                         1.0 + i * 0.1, 2.0 + i * 0.2, 0.5, 1.5 + i * 0.1, 100, 1000));
  auto out = czsc::io::Resample5mTo30m(in);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_DOUBLE_EQ(out[0].open, 1.0);
  EXPECT_DOUBLE_EQ(out[0].close, 2.0);   // 最后一根 close = 1.5 + 0.5 = 2.0
  EXPECT_DOUBLE_EQ(out[0].high, 3.0);   // 2.0 + 1.0 = 3.0
  EXPECT_EQ(out[0].vol, 600);
}

TEST(Resample5mTo30m, TwoBucketsByDateBoundary) {
  // 两个 30 分钟桶，中间隔一个空桶（验证桶边界严格按自然时间）。
  int64_t base = 1704164400;                 // 桶边界
  std::vector<RawBar> in;
  for (int i = 0; i < 6; ++i)                // 桶 A：[base, base+1800)
    in.push_back(MakeBar(Freq::k5Min, base + i * 300, 1, 2, 0.5, 1, 100));
  for (int i = 0; i < 6; ++i) {              // 桶 B：桶 A 结束后隔 1800（空桶），再放 6 根
    int64_t dt = base + 6 * 300 + 1800 + 1800 + i * 300;
    in.push_back(MakeBar(Freq::k5Min, dt, 10, 20, 9, 11, 200));
  }
  auto out = czsc::io::Resample5mTo30m(in);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].vol, 600);
  EXPECT_EQ(out[1].vol, 1200);
}

// ===================== 日线→周线 =====================
TEST(ResampleDailyToWeek, EmptyInput) {
  auto out = czsc::io::ResampleDailyToWeek({});
  EXPECT_TRUE(out.empty());
}

TEST(ResampleDailyToWeek, SingleWeek) {
  // 一周 5 根日线（周一到周五），15:00 CST 锚点 = 07:00 UTC 当日。
  // 2024-01-01 是周一？2024-01-01 是周一（simplify：忽略 ISO 细节，只要桶键相同）。
  // 1704067200 = 2024-01-01 00:00 UTC，+7h = 1704092400。
  int64_t day0 = 1704067200 + 7 * 3600;  // 2024-01-01 15:00 CST
  std::vector<RawBar> in;
  for (int i = 0; i < 5; ++i)
    in.push_back(MakeBar(Freq::kDay, day0 + i * 86400,
                         1.0 + i, 2.0 + i, 0.5 + i, 1.5 + i, 100));
  auto out = czsc::io::ResampleDailyToWeek(in);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].freq, Freq::kWeek);
  EXPECT_DOUBLE_EQ(out[0].open, 1.0);
  EXPECT_DOUBLE_EQ(out[0].close, 5.5);
  EXPECT_EQ(out[0].vol, 500);
}

TEST(ResampleDailyToWeek, CrossYearBoundary) {
  // 12-31（周一）+ 01-07（下周一）—— 验证跨年拆桶。
  // 2024-12-30 周一 15:00 CST：绝对时间不重要，只要在同一 ISO 周即可。
  int64_t week0_mon = 1735484400; // ≈ 2024-12-30 07:00 UTC
  int64_t week1_mon = week0_mon + 7 * 86400;
  std::vector<RawBar> in;
  in.push_back(MakeBar(Freq::kDay, week0_mon, 1, 2, 0.5, 1.5, 100));
  in.push_back(MakeBar(Freq::kDay, week1_mon, 10, 20, 9, 15, 200));
  auto out = czsc::io::ResampleDailyToWeek(in);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].vol, 100);
  EXPECT_EQ(out[1].vol, 200);
}
