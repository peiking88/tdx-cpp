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
