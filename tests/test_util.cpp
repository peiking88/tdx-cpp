// util 单元测试：iconv GBK 转码、zlib 解压、trim_null、ParseMarketCode、时间工具。
#include "tdx/util/gbk.hpp"
#include "tdx/util/time_util.hpp"
#include "tdx/util/zlib_wrap.hpp"
#include "tdx/consts.hpp"
#include "tdx/data/scaling.hpp"

#include <gtest/gtest.h>

#include <zlib.h>

#include <cstring>
#include <string>
#include <vector>

using namespace tdx;

TEST(GbkToUtf8, AsciiPassthrough) {
  EXPECT_EQ(util::gbk_to_utf8("hello"), "hello");
}

TEST(GbkToUtf8, ChineseCharZhong) {
  // "中" GBK = D6 D0，UTF8 = E4 B8 AD
  uint8_t gbk[] = {0xD6, 0xD0};
  std::string utf8 = util::gbk_to_utf8(reinterpret_cast<const char*>(gbk), 2);
  EXPECT_EQ(utf8, "\xE4\xB8\xAD");
}

TEST(GbkToUtf8, LargeInputNoTruncation) {
  // 回归保护：输出 >256 字节时 iconv 返回 E2BIG，须 continue 续转，不能 break 截断。
  // 旧代码误把 E2BIG 当致命错，导致 F10 全文等大 GBK 输入被截断到 ~85 字符。
  // 「中」GBK=D6D0 → UTF8=E4B8AD（3字节）；200 个「中」=400 GBK → 600 UTF8 字节。
  std::string gbk;
  for (int i = 0; i < 200; ++i) gbk += "\xD6\xD0";
  std::string utf8 = util::gbk_to_utf8(gbk);
  EXPECT_EQ(utf8.size(), 600u);
  std::string expected;
  for (int i = 0; i < 200; ++i) expected += "\xE4\xB8\xAD";
  EXPECT_EQ(utf8, expected);
}

TEST(TrimNull, StripsTrailingNull) {
  std::string s = std::string("abc\0def", 7);
  EXPECT_EQ(util::trim_null(s), "abc");
}

TEST(TrimNull, NoNull) {
  EXPECT_EQ(util::trim_null("abc"), "abc");
}

TEST(ZlibInflate, RoundTrip) {
  std::string data = "hello world hello world hello world";
  uLong bound = compressBound(static_cast<uLong>(data.size()));
  std::vector<Bytef> compressed(bound);
  uLongf clen = static_cast<uLongf>(bound);
  int rc = compress(compressed.data(), &clen,
                    reinterpret_cast<const Bytef*>(data.data()),
                    static_cast<uLong>(data.size()));
  ASSERT_EQ(rc, Z_OK);

  auto out = util::zlib_inflate(reinterpret_cast<const uint8_t*>(compressed.data()),
                                static_cast<std::size_t>(clen));
  std::string result(out.begin(), out.end());
  EXPECT_EQ(result, data);
}

TEST(ZlibInflate, EmptyInput) {
  auto out = util::zlib_inflate(nullptr, 0);
  EXPECT_TRUE(out.empty());
}

TEST(ParseMarketCode, WithPrefix) {
  EXPECT_EQ(ParseMarketCode("sh600000").first, Market::SH);
  EXPECT_EQ(ParseMarketCode("sh600000").second, "600000");
  EXPECT_EQ(ParseMarketCode("sz000001").first, Market::SZ);
  EXPECT_EQ(ParseMarketCode("bj430047").first, Market::BJ);
}

TEST(ParseMarketCode, NoPrefix) {
  auto [m, c] = ParseMarketCode("000001");
  EXPECT_TRUE(c.empty());  // 无前缀，code 空表示无效
}

TEST(ClassifySecurity, Index399xxx) {
  using tdx::data::ClassifySecurity;
  using tdx::data::SecurityClass;
  EXPECT_EQ(ClassifySecurity("399001"), SecurityClass::Index);
  EXPECT_EQ(ClassifySecurity("399006"), SecurityClass::Index);
}

TEST(ClassifySecurity, NotIndex390xxx) {
  // P1 修复：390xxx 非指数，应归 AStock（否则 vipdoc 量缩放差 100x）
  using tdx::data::ClassifySecurity;
  using tdx::data::SecurityClass;
  EXPECT_EQ(ClassifySecurity("390001"), SecurityClass::AStock);
  EXPECT_EQ(ClassifySecurity("390999"), SecurityClass::AStock);
}

TEST(ClassifySecurity, BoardIndex) {
  using tdx::data::ClassifySecurity;
  using tdx::data::SecurityClass;
  EXPECT_EQ(ClassifySecurity("880001"), SecurityClass::Index);
  EXPECT_EQ(ClassifySecurity("999999"), SecurityClass::Index);
}

TEST(ClassifySecurity, Funds) {
  using tdx::data::ClassifySecurity;
  using tdx::data::SecurityClass;
  EXPECT_EQ(ClassifySecurity("510050"), SecurityClass::SHFund);
  EXPECT_EQ(ClassifySecurity("159915"), SecurityClass::SZFund);
}

TEST(TimeUtil, CstToEpoch) {
  // 2024-01-01 15:00 CST = 2024-01-01 07:00 UTC = 1704092400
  EXPECT_EQ(util::cst_to_epoch(2024, 1, 1, 15, 0), 1704092400LL);
  EXPECT_EQ(util::date_to_epoch(2024, 1, 1), 1704092400LL);
}

TEST(TimeUtil, EpochRoundTrip) {
  int64_t e = util::cst_to_epoch(2024, 6, 15, 9, 30);
  auto c = util::epoch_to_cst(e);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 6);
  EXPECT_EQ(c.day, 15);
  EXPECT_EQ(c.hour, 9);
  EXPECT_EQ(c.minute, 30);
}
