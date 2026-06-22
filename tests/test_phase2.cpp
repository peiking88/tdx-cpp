// Phase 2 parser 测试：扩展 K线 float32 + SP K线 + 位图 + SP 工具（内联构造验证）。
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "tdx/proto/bitmap.hpp"
#include "tdx/proto/ex_parsers.hpp"
#include "tdx/proto/sp_codec.hpp"
#include "tdx/proto/sp_parsers.hpp"

using namespace tdx::proto;

namespace {
void pu16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(v & 0xff);
  b.push_back((v >> 8) & 0xff);
}
void pu32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff);
}
void pf32(std::vector<uint8_t>& b, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  pu32(b, v);
}
}  // namespace

// ---------- 扩展 K线 float32（OHLC 顺序 open/high/low/close）----------
TEST(ExKline, Float32Ohlc) {
  std::vector<uint8_t> data;
  data.push_back(31);  // market HK_MAIN_BOARD
  for (int i = 0; i < 9; ++i) data.push_back(0);  // code 9s
  pu16(data, 4);                                    // period DAILY
  pu16(data, 1);                                    // times
  pu32(data, 0);                                    // start
  pu16(data, 1);                                    // count
  // 32B：date I, open f, high f, low f, close f, amount f, vol I, _ f
  pu32(data, 20240101);
  pf32(data, 100.5f);    // open
  pf32(data, 101.0f);    // high
  pf32(data, 99.5f);     // low
  pf32(data, 100.8f);    // close
  pf32(data, 1000000.0f);
  pu32(data, 50000);     // vol
  pf32(data, 0.0f);
  auto bars = deserialize_ex_kline(data.data(), data.size(), tdx::Period::DAILY);
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_FLOAT_EQ(static_cast<float>(bars[0].open), 100.5f);
  EXPECT_FLOAT_EQ(static_cast<float>(bars[0].high), 101.0f);
  EXPECT_FLOAT_EQ(static_cast<float>(bars[0].low), 99.5f);
  EXPECT_FLOAT_EQ(static_cast<float>(bars[0].close), 100.8f);
}

TEST(ExLogin, BodyIs80Bytes) {
  auto body = serialize_ex_login();
  EXPECT_EQ(body.size(), 80u);
}

// ---------- SP K线 36B/根 ----------
TEST(SpSymbolBar, Parse36B) {
  std::vector<uint8_t> data;
  pu16(data, 1);  // market
  for (int i = 0; i < 12; ++i) data.push_back(0);  // symbol 12s
  for (int i = 0; i < 10; ++i) data.push_back(0);  // 10x
  data.push_back(4);                                // period DAILY
  pu16(data, 0);                                    // _
  pu16(data, 1);                                    // count
  pu32(data, 0);                                    // start
  // 36B <II7f>
  pu32(data, 20240615);  // ymd
  pu32(data, 34200);     // time_num（9:30 当日秒）
  pf32(data, 10.5f);     // open
  pf32(data, 10.6f);     // high
  pf32(data, 10.4f);     // low
  pf32(data, 10.55f);    // close
  pf32(data, 100000.0f); // amount
  pf32(data, 5000.0f);   // vol
  pf32(data, 0.0f);      // float_shares
  auto bars = deserialize_sp_symbol_bar(data.data(), data.size(), tdx::Period::DAILY);
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_FLOAT_EQ(static_cast<float>(bars[0].open), 10.5f);
  EXPECT_FLOAT_EQ(static_cast<float>(bars[0].close), 10.55f);
}

// ---------- 位图构造 + 解析 ----------
TEST(Bitmap, BuildBasicPreset) {
  auto bm = build_bitmap(PresetBasic());
  // Basic = PRE_CLOSE(0)|OPEN(1)|HIGH(2)|LOW(3)|CLOSE(4)|VOL(5) → byte0 低 6 位 = 0x3f
  EXPECT_EQ(bm[0], 0x3f);
}

TEST(Bitmap, DeserializeDynamicFields) {
  // 构造响应：20B 位图 + <IH>(total, row_count) + 行(68 + 4*popcount)
  std::vector<uint8_t> data(20, 0);
  data[0] = 0x03;  // bit0(pre_close) + bit1(open) → popcount=2
  pu32(data, 1);   // total
  pu16(data, 1);   // row_count
  // 行：<H22s44s>=68 + 2×4B 字段
  pu16(data, 1);                       // market
  for (int i = 0; i < 22; ++i) data.push_back(0);  // symbol
  for (int i = 0; i < 44; ++i) data.push_back(0);  // name
  pf32(data, 10.5f);   // pre_close
  pf32(data, 11.0f);   // open
  auto rows = deserialize_symbol_quotes(data.data(), data.size());
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows[0].fields.size(), 2u);
  EXPECT_EQ(rows[0].fields[0].first, "pre_close");
  EXPECT_FLOAT_EQ(static_cast<float>(rows[0].fields[0].second), 10.5f);
  EXPECT_EQ(rows[0].fields[1].first, "open");
}

// ---------- SP 资金流 JSON ----------
TEST(SpCapitalFlow, ParseJson) {
  // 构造 27B 头 + JSON [[100, 50, 30, 20], [200, 80, 10, 5, 3, 2]]
  std::vector<uint8_t> data(27, 0);
  const char* json = "[[100, 50, 30, 20], [200, 80, 10, 5, 3, 2]]";
  for (const char* p = json; *p; ++p) data.push_back(static_cast<uint8_t>(*p));
  auto cf = deserialize_sp_capital_flow(data.data(), data.size());
  ASSERT_EQ(cf.size(), 1u);
  EXPECT_DOUBLE_EQ(cf[0].main_net, 50.0);      // 100-50
  EXPECT_DOUBLE_EQ(cf[0].small_net, 10.0);     // 30-20
  EXPECT_DOUBLE_EQ(cf[0].five_day_main, 120.0); // 200-80
}
