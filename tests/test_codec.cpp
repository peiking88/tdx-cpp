// codec 单元测试：get_price 变长解码（D4）+ to_datetime 紧凑日期（D5）全边界。
// 对齐 opentdx utils/help.py:137-207，逐用例手算预期。
#include "tdx/proto/codec.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "tdx/util/time_util.hpp"

using namespace tdx::proto;

TEST(GetPrice, SingleBytePositive) {
  uint8_t d[] = {0x05};  // low6=5, no sign, no continue
  auto r = get_price(d, 1, 0);
  EXPECT_EQ(r.value, 5);
  EXPECT_EQ(r.new_pos, 1u);
}

TEST(GetPrice, SingleByteNegative) {
  uint8_t d[] = {0x45};  // 0x40 sign bit, low6=5
  auto r = get_price(d, 1, 0);
  EXPECT_EQ(r.value, -5);
  EXPECT_EQ(r.new_pos, 1u);
}

TEST(GetPrice, TwoBytePositive) {
  uint8_t d[] = {0x80, 0x01};  // continue, second byte 1<<6 = 64
  auto r = get_price(d, 2, 0);
  EXPECT_EQ(r.value, 64);
  EXPECT_EQ(r.new_pos, 2u);
}

TEST(GetPrice, TwoByteNegative) {
  uint8_t d[] = {0xC0, 0x01};  // sign + continue, 1<<6 = 64 → -64
  auto r = get_price(d, 2, 0);
  EXPECT_EQ(r.value, -64);
  EXPECT_EQ(r.new_pos, 2u);
}

TEST(GetPrice, MultiByteChain) {
  // 0xFF 0x7F: int_data=0x3f=63 + (0x7f)<<6 = 8128 → 8191, sign → -8191
  uint8_t d[] = {0xFF, 0x7F};
  auto r = get_price(d, 2, 0);
  EXPECT_EQ(r.value, -8191);
  EXPECT_EQ(r.new_pos, 2u);
}

TEST(GetPrice, OutOfBounds) {
  uint8_t d[] = {0x05};
  auto r = get_price(d, 1, 5);  // pos >= data_len
  EXPECT_EQ(r.value, 0);
  EXPECT_EQ(r.new_pos, 6u);
}

TEST(GetPrice, ContinueThenTruncate) {
  // 0x80 继续位 set，但后续无字节 → 越界 break，int_data 保持首字节低6位
  uint8_t d[] = {0x80};
  auto r = get_price(d, 1, 0);
  EXPECT_EQ(r.value, 0);  // 0x80 & 0x3f = 0
}

TEST(GetPrice, MaxContByteCap) {
  // 12 个续字节链：cap=9 应在第 9 个续字节后停止，防止 << 64 UB（P1 修复）。
  // 所有续字节 data=0，值仅来自首字节低 6 位。
  uint8_t d[14];
  d[0] = 0x80 | 0x01;  // continue=1, sign=0, data=1
  for (int i = 1; i <= 12; ++i) d[i] = 0x80;  // 12 个 continue+data=0
  d[13] = 0x00;
  auto r = get_price(d, sizeof(d), 0);
  EXPECT_EQ(r.value, 1);  // 首字节 data=1 + 续字节全 0
}

TEST(GetPrice, LargeNegativeNoOverflow) {
  // sign + 5 个续字节 → 大负数，验证取负不 UB（P1 修复：INT64_MIN 保护）。
  // 编码：0xFF=sign+continue+data(0x3F=63), 4×0xFF=continue+data(0x7F=127), 0x7F=no continue+data=127
  // int_data = 63 + 127<<6 + 127<<13 + 127<<20 + 127<<27 + 127<<34 = 2199023255551
  // sign → -2199023255551
  uint8_t d[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
  auto r = get_price(d, sizeof(d), 0);
  EXPECT_EQ(r.value, -2199023255551LL);
}

// format_time_to_epoch — server_time HHMMSSmm 解析回归（v0.13.1 修复，v0.13.2 加固）
TEST(FormatTimeToEpoch, NormalTime) {
  // 9301500 = 09:30:15.00 → 当日 09:30 CST epoch（秒部分丢弃，仅到分钟）
  int64_t e = format_time_to_epoch(9301500);
  EXPECT_GT(e, 0) << "应返回有效 epoch";
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.hour, 9);
  EXPECT_EQ(c.minute, 30);
  // 秒分量不参与 epoch（对齐 Python format_time）
}

TEST(FormatTimeToEpoch, Zero) {
  EXPECT_EQ(format_time_to_epoch(0), 0);
}

TEST(FormatTimeToEpoch, Hundred) {
  EXPECT_EQ(format_time_to_epoch(100), 0);  // ts==100 哨兵 → 0
}

TEST(FormatTimeToEpoch, TooShort) {
  EXPECT_EQ(format_time_to_epoch(1), 0);       // 1 位 < 7 → 0
  EXPECT_EQ(format_time_to_epoch(12345), 0);   // 5 位 < 7 → 0
}

TEST(ToDatetime, DailyYYYYMMDD) {
  int64_t e = to_datetime(20240101, false);  // 2024-01-01 15:00 CST
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 1);
  EXPECT_EQ(c.day, 1);
  EXPECT_EQ(c.hour, 15);
  EXPECT_EQ(c.minute, 0);
}

TEST(ToDatetime, CompactWithTime) {
  // 构造紧凑编码：2024-06-15 09:30
  int zip_data = (2024 - 2004) << 11 | (6 * 100 + 15);  // 20<<11 | 615 = 41575
  int minutes = 9 * 60 + 30;                              // 570
  int64_t num = (static_cast<int64_t>(minutes) << 16) | zip_data;
  int64_t e = to_datetime(num, true);
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 6);
  EXPECT_EQ(c.day, 15);
  EXPECT_EQ(c.hour, 9);
  EXPECT_EQ(c.minute, 30);
}

TEST(ToDatetime, DailyFallbackToCompact) {
  // with_time=false, month=13 越界 → 回退紧凑编码（D5 fallback）
  // num=20241301 & 0xFFFF = 56213 → year=(56213>>11)+2004=2031, month=9, day=17
  int64_t e = to_datetime(20241301, false);
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.year, 2031);
  EXPECT_EQ(c.month, 9);
  EXPECT_EQ(c.day, 17);
  EXPECT_EQ(c.hour, 15);
}

TEST(ToDatetime, CompactFallbackToDaily) {
  // with_time=true，构造一个紧凑解出越界的 num，应回退 YYYYMMDD
  // num = 20240615（YYYYMMDD 形式），with_time=true 时紧凑解出 month 越界 → 回退
  int64_t e = to_datetime(20240615, true);
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 6);
  EXPECT_EQ(c.day, 15);
}
