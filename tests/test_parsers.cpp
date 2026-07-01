// parser 单元测试：内联构造响应字节（用 encode_price 反向编码），验证解析。
// 覆盖黄金字节流难点：D3（OHLC 顺序）、D1（增量累加）、D2（相对基准）、固定格式。
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "tdx/proto/codec.hpp"
#include "tdx/proto/parsers.hpp"
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
// 反向 get_price 编码（测试用，构造变长字段）
std::vector<uint8_t> encode_price(int64_t v) {
  std::vector<uint8_t> out;
  bool sign = v < 0;
  uint64_t mag = sign ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
  uint8_t first = mag & 0x3f;
  mag >>= 6;
  if (sign) first |= 0x40;
  if (mag > 0) first |= 0x80;
  out.push_back(first);
  while (mag > 0) {
    uint8_t bb = mag & 0x7f;
    mag >>= 7;
    if (mag > 0) bb |= 0x80;
    out.push_back(bb);
  }
  return out;
}
void ep(std::vector<uint8_t>& b, int64_t v) {
  auto e = encode_price(v);
  b.insert(b.end(), e.begin(), e.end());
}
}  // namespace

// ---------- K 线：OHLC 顺序（D3）----------
TEST(KlineParser, OhlcOrder) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);          // count
  pu32(resp, 20240101);   // date YYYYMMDD
  ep(resp, 1000);         // open
  ep(resp, 1010);         // close（在 high 前！）
  ep(resp, 1020);         // high
  ep(resp, 990);          // low
  pf32(resp, 100000.0f);  // vol
  pf32(resp, 5000000.0f); // amount
  auto bars = deserialize_kline(resp.data(), resp.size(), Period::DAILY);
  ASSERT_EQ(bars.size(), 1u);
  // get_price 原始值 /1000（对齐 quotationClient.py:133-136）
  EXPECT_DOUBLE_EQ(bars[0].open, 1.0);
  EXPECT_DOUBLE_EQ(bars[0].close, 1.01);
  EXPECT_DOUBLE_EQ(bars[0].high, 1.02);
  EXPECT_DOUBLE_EQ(bars[0].low, 0.99);
  EXPECT_DOUBLE_EQ(bars[0].volume, 100000.0);
  auto c = util::epoch_to_cst(bars[0].datetime);
  EXPECT_EQ(c.year, 2024);
  EXPECT_EQ(c.month, 1);
  EXPECT_EQ(c.day, 1);
}

TEST(KlineParser, EmptyCount) {
  std::vector<uint8_t> resp;
  pu16(resp, 0);
  auto bars = deserialize_kline(resp.data(), resp.size(), Period::DAILY);
  EXPECT_TRUE(bars.empty());
}

// ---------- 逐笔：price 增量累加（D1）----------
TEST(TransactionParser, PriceIncrement) {
  std::vector<uint8_t> resp;
  pu16(resp, 2);
  // 条1：minutes=570(9:30) price=+1000 vol=100 trans=1 buy_sell=0(BUY) unknown=0
  pu16(resp, 570);
  ep(resp, 1000); ep(resp, 100); ep(resp, 1); ep(resp, 0); ep(resp, 0);
  // 条2：minutes=571 price=+10(增量) vol=50 trans=2 buy_sell=1(SELL) unknown=0
  pu16(resp, 571);
  ep(resp, 10); ep(resp, 50); ep(resp, 2); ep(resp, 1); ep(resp, 0);
  auto txns = deserialize_transaction(resp.data(), resp.size());
  ASSERT_EQ(txns.size(), 2u);
  // price 增量累加后 /100（对齐 quotationClient.py:199）
  EXPECT_DOUBLE_EQ(txns[0].price, 10.0);     // 0 + 1000 → /100 = 10.0
  EXPECT_EQ(txns[0].buy_sell, BuySell::Buy);
  EXPECT_DOUBLE_EQ(txns[1].price, 10.1);     // (1000+10) / 100 = 10.1
  EXPECT_EQ(txns[1].buy_sell, BuySell::Sell);
}

// ---------- 分时：双增量（D1）----------
TEST(TickParser, DoubleIncrement) {
  std::vector<uint8_t> resp;
  pu16(resp, 2);  // num（前 4 字节两 H，第一个=num）
  pu16(resp, 0);  // 第二个 H 丢弃
  // 条1：price=1000 avg=1005 vol=100 → 输出 start=0: price=1000 avg=1005
  ep(resp, 1000); ep(resp, 1005); ep(resp, 100);
  // 条2：price=10(增量) avg=15(增量) vol=50 → 输出 1000+10=1010, 1005+15=1020
  ep(resp, 10); ep(resp, 15); ep(resp, 50);
  auto ticks = deserialize_tick(resp.data(), resp.size());
  ASSERT_EQ(ticks.size(), 2u);
  // price/100, avg/10000（对齐 quotationClient.py:150-151）
  EXPECT_DOUBLE_EQ(ticks[0].price, 10.0);
  EXPECT_DOUBLE_EQ(ticks[0].avg,   0.1005);
  EXPECT_DOUBLE_EQ(ticks[1].price, 10.1);    // (1000+10)/100 = 10.1
  EXPECT_DOUBLE_EQ(ticks[1].avg,   0.1020);  // (1005+15)/10000 = 0.102
}

// ---------- 五档：price 基准 + OHLC/五档相对增量（D2）----------
TEST(QuotesParser, BaseAndRelativeIncrement) {
  std::vector<uint8_t> resp;
  pu16(resp, 0);   // 头部第一个 H 丢弃
  pu16(resp, 1);   // count
  // <B6sH>：market=1, code="600000", active1=0
  resp.push_back(1);
  for (char ch : std::string("600000")) resp.push_back(ch);
  pu16(resp, 0);
  // price 基准=1000
  ep(resp, 1000);          // price（基准）
  ep(resp, -10);           // pre_close 增量 → 1000-10=990
  ep(resp, 0);             // open → 1000+0=1000
  ep(resp, 20);            // high → 1000+20=1020
  ep(resp, -5);            // low → 1000-5=995
  ep(resp, 0);             // server_time
  ep(resp, 0);             // neg_price
  ep(resp, 1000);          // vol
  ep(resp, 10);            // cur_vol
  pf32(resp, 500000.0f);   // amount（float32）
  ep(resp, 0); ep(resp, 0); ep(resp, 0); ep(resp, 0);  // s_vol b_vol s_amount open_amount
  // 五档 ×5：bid/ask 相对 price 增量
  for (int j = 0; j < 5; ++j) {
    ep(resp, -j);    // bid 增量 → 1000-j
    ep(resp, j + 1); // ask 增量 → 1000+j+1
    ep(resp, 100);   // bid_vol
    ep(resp, 200);   // ask_vol
  }
  // 尾部 10 字节
  for (int i = 0; i < 10; ++i) resp.push_back(0);

  auto quotes = deserialize_quotes_detail(resp.data(), resp.size());
  ASSERT_EQ(quotes.size(), 1u);
  // OHLC/price/pre_close/bid/ask /100, amount *100（对齐 quotationClient.py:36-42）
  EXPECT_DOUBLE_EQ(quotes[0].price,     10.0);   // 1000 / 100
  EXPECT_DOUBLE_EQ(quotes[0].pre_close,  9.9);   // (1000-10) / 100
  EXPECT_DOUBLE_EQ(quotes[0].open,      10.0);
  EXPECT_DOUBLE_EQ(quotes[0].high,      10.2);   // (1000+20) / 100
  EXPECT_DOUBLE_EQ(quotes[0].low,        9.95);  // (1000-5) / 100
  EXPECT_DOUBLE_EQ(quotes[0].bid[0],    10.0);   // (1000+0) / 100
  EXPECT_DOUBLE_EQ(quotes[0].ask[0],    10.01);  // (1000+1) / 100
  EXPECT_DOUBLE_EQ(quotes[0].bid[1],     9.99);  // (1000-1) / 100
  EXPECT_DOUBLE_EQ(quotes[0].ask[1],    10.02);  // (1000+2) / 100
  EXPECT_DOUBLE_EQ(quotes[0].amount, 500000.0);  // amount 不缩放（NetQuotes）
  EXPECT_EQ(quotes[0].code, "600000");
}

// ---------- 列表：固定 37B（GBK name + decimal_point）----------
TEST(ListParser, FixedRecord) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);
  std::string code = "600000";
  std::string name = "PFYH";  // ASCII 简化
  for (int i = 0; i < 6; ++i) resp.push_back(i < (int)code.size() ? code[i] : 0);
  pu16(resp, 1000);  // vol
  for (int i = 0; i < 16; ++i) resp.push_back(i < (int)name.size() ? name[i] : 0);
  pf32(resp, 0.0f);      // unknown1
  resp.push_back(2);     // decimal_point
  pf32(resp, 10.5f);     // pre_close
  pu16(resp, 0);         // unknown2
  pu16(resp, 0);         // unknown3
  auto stocks = deserialize_list(resp.data(), resp.size());
  ASSERT_EQ(stocks.size(), 1u);
  EXPECT_EQ(stocks[0].code, "600000");
  EXPECT_EQ(stocks[0].name, "PFYH");
  EXPECT_EQ(stocks[0].decimal_point, 2);
  EXPECT_FLOAT_EQ(static_cast<float>(stocks[0].pre_close), 10.5f);
}

TEST(CountParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 5000);
  EXPECT_EQ(deserialize_count(resp.data(), resp.size()), 5000);
}

// ---------- 请求构造 ----------
TEST(Serialize, KlineRequestLayout) {
  auto body = serialize_kline(Market::SH, "600000", Period::DAILY, 1, 0, 800, Adjust::NONE);
  // <H6sHHHHH8s>：market@0 code@2 period@8 times@10 start@12 count@14 adjust@16 8s@18 = 26 字节
  EXPECT_EQ(body.size(), 26u);
  EXPECT_EQ(body[0], 0x01);   // market SH=1
  EXPECT_EQ(body[8], 0x04);   // period DAILY=4
  EXPECT_EQ(body[12], 0x00);  // start=0 低字节
  EXPECT_EQ(body[14], 0x20);  // count=800=0x0320 低字节
  EXPECT_EQ(body[15], 0x03);  // count 高字节
}

TEST(Serialize, LoginHeartbeat) {
  auto login = serialize_login();
  EXPECT_EQ(login.size(), 1u);
  EXPECT_EQ(login[0], 0x01);
  auto hb = serialize_heartbeat();
  EXPECT_TRUE(hb.empty());
}

// ---------- 错误路径：空/截断数据 ----------
TEST(KlineParser, EmptyData) {
  std::vector<uint8_t> empty;
  auto bars = deserialize_kline(empty.data(), 0, Period::DAILY);
  EXPECT_TRUE(bars.empty());
}

TEST(KlineParser, TruncatedHeader) {
  uint8_t d[] = {0x01};  // count=1 但无后续数据
  auto bars = deserialize_kline(d, 1, Period::DAILY);
  EXPECT_TRUE(bars.empty());
}

TEST(KlineParser, TruncatedBody) {
  std::vector<uint8_t> d;
  pu16(d, 1);           // count=1
  pu32(d, 20240101);     // date
  // 缺 OHLC → pos 越界 break，bar vector 为空
  auto bars = deserialize_kline(d.data(), d.size(), Period::DAILY);
  EXPECT_TRUE(bars.empty());
}

TEST(TickParser, EmptyData) {
  auto ticks = deserialize_tick(nullptr, 0);
  EXPECT_TRUE(ticks.empty());
}

TEST(TickParser, Truncated) {
  uint8_t d[] = {0x01};  // 仅 1 字节，len < 4 → 返回空
  auto ticks = deserialize_tick(d, 1);
  EXPECT_TRUE(ticks.empty());
}

TEST(TransactionParser, EmptyData) {
  auto txns = deserialize_transaction(nullptr, 0);
  EXPECT_TRUE(txns.empty());
}

TEST(TransactionParser, Truncated) {
  uint8_t d[] = {0x01, 0x00};  // count=1, 但无字段
  auto txns = deserialize_transaction(d, 2);
  EXPECT_TRUE(txns.empty());
}

TEST(ListParser, EmptyData) {
  auto stocks = deserialize_list(nullptr, 0);
  EXPECT_TRUE(stocks.empty());
}

TEST(CountParser, EmptyData) {
  EXPECT_EQ(deserialize_count(nullptr, 0), 0);
}

TEST(Serialize, CodeTruncation) {
  // code 超 6 字节 → push_code6 截断前 6 字节
  auto body = serialize_kline(Market::SH, "6000001234", Period::DAILY, 1, 0, 100, Adjust::NONE);
  EXPECT_EQ(body.size(), 26u);
  // 验证 code 位置仅含前 6 字节 '600000'（offset 2-7），period 在 offset 8
  EXPECT_EQ(body[2], '6');
  EXPECT_EQ(body[7], '0');  // code[5]
  EXPECT_EQ(body[8], 0x04); // period DAILY=4
}
