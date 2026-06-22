// sp_codec 单元测试：exchange_board_code 板块码转换 + combine_to_datetime 日期合成。
#include <gtest/gtest.h>

#include "tdx/proto/sp_codec.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx::proto;

TEST(SpCodec, ExchangeBoardCode) {
  // 对齐 help.py:66-89 各分支注释的预期值
  EXPECT_EQ(exchange_board_code("US0401"), 30401);
  EXPECT_EQ(exchange_board_code("HK0283"), 20283);
  EXPECT_EQ(exchange_board_code("000686"), 31686);
  EXPECT_EQ(exchange_board_code("399372"), 30372);  // 399372-399000+30000
  EXPECT_EQ(exchange_board_code("899050"), 32050);  // 899050-899000+32000
  EXPECT_EQ(exchange_board_code("880686"), 20686);  // 880686-880000+20000
}

TEST(SpCodec, CombineToDatetime) {
  // 2024-06-15 当日 9:30 = 9*3600+30*60 = 34200 秒
  int64_t e = combine_to_datetime(20240615, 34200);
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 6);
  EXPECT_EQ(c.day, 15);
  EXPECT_EQ(c.hour, 9);
  EXPECT_EQ(c.minute, 30);
}

TEST(SpCodec, CombineToDatetimeFuturesOffset) {
  // format_tdx_time=true 且 0-5 点 → 次日（美股/期货偏移）
  int64_t e = combine_to_datetime(20240615, 2 * 3600, true);  // 02:00 → 次日
  auto c = tdx::util::epoch_to_cst(e);
  EXPECT_EQ(c.day, 16);  // +1 天
  EXPECT_EQ(c.hour, 2);
}
