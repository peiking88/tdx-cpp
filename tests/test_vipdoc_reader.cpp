// vipdoc_reader 单元测试：构造 .day/.lc1 字节写临时文件，读回验证缩放与解码。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tdx/proto/vipdoc_reader.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx;
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
void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), data.size());
}
}  // namespace

TEST(VipdocReader, DayFileScaling) {
  std::string tmp = "/home/li/.claude/jobs/030bcd8c/tmp/vipdoc_test";
  std::filesystem::remove_all(tmp);

  std::vector<uint8_t> rec;
  pu32(rec, 20240101);       // date YYYYMMDD
  pu32(rec, 100000);         // open ×0.01 = 1000.00
  pu32(rec, 102000);         // high = 1020.00
  pu32(rec, 99000);          // low = 990.00
  pu32(rec, 101000);         // close = 1010.00
  pf32(rec, 5000000.0f);     // amount（不缩放）
  pu32(rec, 10000000);       // volume ×1.0 = 10000000（股）
  pu32(rec, 0);              // reserved

  WriteFile(tmp + "/vipdoc/sh/lday/sh600000.day", rec);

  VipdocReader vr(tmp);
  auto bars = vr.ReadDay(Market::SH, "600000");
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_NEAR(bars[0].open, 1000.00, 0.001);
  EXPECT_NEAR(bars[0].high, 1020.00, 0.001);
  EXPECT_NEAR(bars[0].low, 990.00, 0.001);
  EXPECT_NEAR(bars[0].close, 1010.00, 0.001);
  EXPECT_NEAR(bars[0].volume, 10000000.0, 0.001);
  auto c = util::epoch_to_cst(bars[0].datetime);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 1);
  EXPECT_EQ(c.day, 1);
}

TEST(VipdocReader, Min1FileDecode) {
  std::string tmp = "/home/li/.claude/jobs/030bcd8c/tmp/vipdoc_test2";
  std::filesystem::remove_all(tmp);

  // 构造紧凑日期：2024-06-15。date_h = (2024-2004)*2048 + (6*100+15) = 20*2048 + 615 = 41575
  // time_h = 9*60+30 = 570
  std::vector<uint8_t> rec;
  pu16(rec, 41575);         // date 紧凑
  pu16(rec, 570);           // time 紧凑
  pf32(rec, 10.50f);        // open
  pf32(rec, 10.60f);        // high
  pf32(rec, 10.40f);        // low
  pf32(rec, 10.55f);        // close
  pf32(rec, 100000.0f);     // amount
  pu32(rec, 5000);          // volume
  pu32(rec, 0);             // reserved

  WriteFile(tmp + "/vipdoc/sh/minline/sh600000.lc1", rec);

  VipdocReader vr(tmp);
  auto bars = vr.ReadMin1(Market::SH, "600000");
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_NEAR(bars[0].open, 10.50, 0.001);
  EXPECT_NEAR(bars[0].high, 10.60, 0.001);
  EXPECT_NEAR(bars[0].low, 10.40, 0.001);
  EXPECT_NEAR(bars[0].close, 10.55, 0.001);
  auto c = util::epoch_to_cst(bars[0].datetime);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 6);
  EXPECT_EQ(c.day, 15);
  EXPECT_EQ(c.hour, 9);
  EXPECT_EQ(c.minute, 30);
}

TEST(VipdocReader, MissingFileReturnsEmpty) {
  VipdocReader vr("/nonexistent/tdx/path");
  auto bars = vr.ReadDay(Market::SH, "600000");
  EXPECT_TRUE(bars.empty());
}
