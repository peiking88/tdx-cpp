// 黄金字节流测试：加载真服录制的 .bin，C++ 解析须成功且字段合理。
// fixtures 由 scripts/record_golden.py 录制（连真服抓响应 body）。
// 严格逐字段对齐 opentdx expected.json 需 JSON 库，留待后续；此处先验证
// C++ 解析器能正确消化真实字节流（解析出合理数量的非零记录）。
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "tdx/proto/parsers.hpp"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures/golden"
#endif

using namespace tdx;

namespace {
std::vector<uint8_t> ReadBin(const std::string& name) {
  std::string path = std::string(FIXTURE_DIR) + "/" + name + ".bin";
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}
}  // namespace

TEST(GoldenKline, ParseRealServer600000) {
  auto data = ReadBin("kline_600000_day");
  ASSERT_FALSE(data.empty());
  auto bars = proto::deserialize_kline(data.data(), data.size(), Period::DAILY);
  ASSERT_GT(bars.size(), 0u);
  // 字段合理（非零）：opentdx kline 价格为 get_price 原值（整数，未 /100）
  EXPECT_GT(bars[0].open, 0.0);
  EXPECT_GT(bars[0].close, 0.0);
  EXPECT_GT(bars[0].high, 0.0);
  EXPECT_GT(bars[0].low, 0.0);
  EXPECT_GT(bars[0].volume, 0.0);
  // OHLC 顺序正确：high >= max(open,close), low <= min(open,close)
  EXPECT_GE(bars[0].high, std::max(bars[0].open, bars[0].close));
  EXPECT_LE(bars[0].low, std::min(bars[0].open, bars[0].close));
}

TEST(GoldenTick, ParseRealServer600000) {
  auto data = ReadBin("tick_600000");
  ASSERT_FALSE(data.empty());
  auto ticks = proto::deserialize_tick(data.data(), data.size());
  EXPECT_GT(ticks.size(), 0u);
  EXPECT_GT(ticks[0].price, 0.0);
}

TEST(GoldenTransaction, ParseRealServer600000) {
  auto data = ReadBin("transaction_600000");
  ASSERT_FALSE(data.empty());
  auto txns = proto::deserialize_transaction(data.data(), data.size());
  ASSERT_GT(txns.size(), 0u);
  EXPECT_GT(txns[0].price, 0.0);
}

TEST(GoldenQuotes, ParseRealServer600000) {
  auto data = ReadBin("quotes_600000");
  ASSERT_FALSE(data.empty());
  auto quotes = proto::deserialize_quotes_detail(data.data(), data.size());
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_EQ(quotes[0].code, "600000");
  EXPECT_GT(quotes[0].price, 0.0);
  // 五档 bid/ask 应解析（可能部分 NaN：盘前档位）
  EXPECT_GT(quotes[0].bid[0], 0.0);
  EXPECT_GT(quotes[0].ask[0], 0.0);
}
