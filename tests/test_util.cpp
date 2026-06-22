// util 单元测试：iconv GBK 转码、zlib 解压、trim_null、MarketFromCode、时间工具。
#include "tdx/util/gbk.hpp"
#include "tdx/util/time_util.hpp"
#include "tdx/util/zlib_wrap.hpp"
#include "tdx/consts.hpp"

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

TEST(MarketFromCode, Shanghai) {
  EXPECT_EQ(MarketFromCode("600000"), Market::SH);
  EXPECT_EQ(MarketFromCode("688981"), Market::SH);
}

TEST(MarketFromCode, Shenzhen) {
  EXPECT_EQ(MarketFromCode("000001"), Market::SZ);
  EXPECT_EQ(MarketFromCode("300750"), Market::SZ);
}

TEST(MarketFromCode, Beijing) {
  EXPECT_EQ(MarketFromCode("830799"), Market::BJ);
  EXPECT_EQ(MarketFromCode("430047"), Market::BJ);
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
