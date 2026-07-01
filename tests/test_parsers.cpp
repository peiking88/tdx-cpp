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

// ==================== 新增接口 parser 测试 ====================

// ---- 财务 0x10 ----
TEST(FinanceParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);                // type/num
  resp.push_back(1);            // market=SH
  for (char ch : std::string("600000")) resp.push_back(ch);
  pf32(resp, 100000.0f);        // liutongguben
  pu16(resp, 31);               // province
  pu16(resp, 102);              // industry
  pu32(resp, 20260101);         // updated_date
  pu32(resp, 20200601);         // ipo_date
  for (int i = 0; i < 29; ++i) pf32(resp, 1.0f + i);  // 29 float 字段
  auto f = deserialize_finance(resp.data(), resp.size());
  EXPECT_DOUBLE_EQ(f.liutongguben, 100000.0);
  EXPECT_EQ(f.ipo_date, 20200601u);
  EXPECT_EQ(f.industry, 102);
  EXPECT_DOUBLE_EQ(f.meigushouyi, 7.0);   // 29-field index 6 (1.0+6)
  EXPECT_DOUBLE_EQ(f.meigujinzichan, 29.0); // index 28 (1.0+28)
}

TEST(FinanceParser, Truncated) {
  uint8_t d[8] = {};  // < 141B → 空 Finance（code 为空是区分信号）
  auto f = deserialize_finance(d, 8);
  EXPECT_TRUE(f.code.empty());
}

// ---- F10 分类 0x2cf ----
TEST(F10Parser, Category) {
  std::vector<uint8_t> resp;
  pu16(resp, 2);  // count=2
  for (int e = 0; e < 2; ++e) {
    std::string nm = (e == 0) ? "company" : "finance";
    std::string fn = (e == 0) ? "comp.txt" : "fin.txt";
    for (size_t j = 0; j < 64; ++j) resp.push_back(j < nm.size() ? nm[j] : 0);
    for (size_t j = 0; j < 80; ++j) resp.push_back(j < fn.size() ? fn[j] : 0);
    pu32(resp, e * 100);
    pu32(resp, e * 100 + 50);
  }
  auto cats = deserialize_f10_category(resp.data(), resp.size());
  ASSERT_EQ(cats.size(), 2u);
  EXPECT_EQ(cats[0].name, "company");
  EXPECT_EQ(cats[1].name, "finance");
  EXPECT_EQ(cats[0].start, 0u);
  EXPECT_EQ(cats[1].start, 100u);
  EXPECT_EQ(cats[0].length, 50u);
  EXPECT_EQ(cats[1].length, 150u);
}

TEST(F10Parser, Empty) {
  uint8_t d[1] = {0};
  EXPECT_TRUE(deserialize_f10_category(d, 1).empty());
}

// ---- F10 内容 0x2d0（header 12B + GBK content）----
TEST(F10ContentParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);                                    // market=SH
  for (char ch : std::string("600000")) resp.push_back(ch);
  pu16(resp, 7);                                    // category
  pu16(resp, 5);                                    // length=5 (u16，对齐 <H6sHH>)
  for (char ch : std::string("hello")) resp.push_back(ch);  // content
  auto c = deserialize_f10_content(resp.data(), resp.size());
  EXPECT_EQ(c.market, 1);
  EXPECT_EQ(c.code, "600000");
  EXPECT_EQ(c.category, 7);
  EXPECT_EQ(c.length, 5u);
  EXPECT_EQ(c.content, "hello");
}

// F10Content 边界 1: len < 12B → 空（code 为空是区分信号）
TEST(F10ContentParser, Truncated) {
  uint8_t d[8] = {};
  auto c = deserialize_f10_content(d, 8);
  EXPECT_TRUE(c.code.empty());
  EXPECT_EQ(c.length, 0u);
  EXPECT_TRUE(c.content.empty());
}

// F10Content 边界 2: c.length 声称 100 但实际仅 2B content → content 应空（防越界读）
TEST(F10ContentParser, LengthExceedsData) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);
  for (char ch : std::string("600000")) resp.push_back(ch);
  pu16(resp, 7);
  pu16(resp, 100);                                  // length 声称 100 (u16)
  for (char ch : std::string("ab")) resp.push_back(ch);  // 实际仅 2B → 12+100 > len
  auto c = deserialize_f10_content(resp.data(), resp.size());
  EXPECT_EQ(c.code, "600000");
  EXPECT_EQ(c.length, 100u);
  EXPECT_TRUE(c.content.empty());                   // 守卫生效，content 不读
}

// ---- 历史委托 0xfb4 ----
TEST(HistoryOrdersParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 2);              // count=2
  pf32(resp, 10.5f);          // pre_close
  ep(resp, 1000);             // price +=1000
  ep(resp, 0);                // unknown
  ep(resp, 100);              // vol
  ep(resp, 10);               // price +=10 → 1010
  ep(resp, 1);                // unknown
  ep(resp, 50);               // vol
  auto orders = deserialize_history_orders(resp.data(), resp.size());
  ASSERT_EQ(orders.size(), 2u);
  // price /100 (NetTransaction scaling)
  EXPECT_DOUBLE_EQ(orders[0].price, 10.0);   // 1000 * 0.01
  EXPECT_EQ(orders[0].vol, 100);
  EXPECT_DOUBLE_EQ(orders[1].price, 10.1);   // (1000+10) * 0.01
  EXPECT_EQ(orders[1].vol, 50);
}

TEST(HistoryOrdersParser, Empty) {
  EXPECT_TRUE(deserialize_history_orders(nullptr, 0).empty());
}

TEST(HistoryOrdersParser, TruncatedBody) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);               // count=1
  pf32(resp, 10.5f);           // pre_close
  // 无 body → pos=6，count=1 但 get_price 无法读取 → 应 safe return empty
  auto orders = deserialize_history_orders(resp.data(), resp.size());
  EXPECT_TRUE(orders.empty());  // 不应 crash
}

// ---- 历史逐笔 0xfb5 ----
TEST(HistoryTxParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 2);              // count=2
  pf32(resp, 10.5f);          // pre_close
  pu16(resp, 570);            // minutes=9:30
  ep(resp, 1000); ep(resp, 100); ep(resp, 0); ep(resp, 0);
  pu16(resp, 571);            // minutes=9:31
  ep(resp, 10);  ep(resp, 50);  ep(resp, 1); ep(resp, 0);
  auto txns = deserialize_history_transaction(resp.data(), resp.size());
  ASSERT_EQ(txns.size(), 2u);
  EXPECT_DOUBLE_EQ(txns[0].price, 10.0);  // 1000 * 0.01
  EXPECT_EQ(txns[0].minutes, 570);
  EXPECT_EQ(txns[0].buy_sell, 0);
  EXPECT_DOUBLE_EQ(txns[1].price, 10.1);  // 1010 * 0.01
  EXPECT_EQ(txns[1].buy_sell, 1);
}

TEST(HistoryTxParser, Empty) {
  EXPECT_TRUE(deserialize_history_transaction(nullptr, 0).empty());
}

// ---- 成交量分布 0x51a ----
TEST(VolProfileParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 2);              // count=2 levels
  resp.push_back(1);          // market=SH
  for (char ch : std::string("600000")) resp.push_back(ch);
  pu16(resp, 0);              // active
  // 基准 price=1000
  ep(resp, 1000);  ep(resp, -10);  ep(resp, 0);  ep(resp, 20);  ep(resp, -5);
  ep(resp, 0);     ep(resp, 0);    ep(resp, 500); ep(resp, 10);
  pf32(resp, 500000.0f);     // amount
  ep(resp, 0); ep(resp, 0); ep(resp, 0); ep(resp, 0);  // s_vol etc
  for (int j = 0; j < 3; ++j) { ep(resp, 0); ep(resp, 0); ep(resp, 0); ep(resp, 0); }
  pu16(resp, 0);              // unknown
  // 2 levels
  ep(resp, 1000); ep(resp, 500); ep(resp, 200); ep(resp, 300);
  ep(resp, 10);   ep(resp, 400); ep(resp, 250); ep(resp, 150);
  auto vp = deserialize_volume_profile(resp.data(), resp.size());
  EXPECT_EQ(vp.code, "600000");
  // pre_close = (1000-10)/100 = 9.9
  EXPECT_DOUBLE_EQ(vp.pre_close, 9.9);
  EXPECT_DOUBLE_EQ(vp.price, 10.0);   // 1000 / 100
  ASSERT_EQ(vp.levels.size(), 2u);
  EXPECT_EQ(vp.levels[0].vol, 500);
  EXPECT_EQ(vp.levels[1].vol, 400);
  EXPECT_EQ(vp.levels[0].buy, 200);
  EXPECT_EQ(vp.levels[0].sell, 300);
  EXPECT_EQ(vp.levels[1].buy, 250);
  EXPECT_EQ(vp.levels[1].sell, 150);
}

TEST(VolProfileParser, Empty) {
  VolProfile vp = deserialize_volume_profile(nullptr, 0);
  EXPECT_TRUE(vp.code.empty());
}

// ---- 指数信息 0x51d ----
TEST(IndexInfoParser, Basic) {
  std::vector<uint8_t> resp;
  pu32(resp, 2);              // count = 2 orders
  resp.push_back(1);          // market=SH
  for (char ch : std::string("399001")) resp.push_back(ch);
  pu16(resp, 0);              // active
  // close=4000 base, then 17×get_price
  // close(4000), pre_close_diff(-10→3990), open_diff(5→4005), high_diff(30→4030), low_diff(-20→3980)
  // server_t(0), after_h(0), vol(1000), cur_vol(5)
  ep(resp, 4000); ep(resp, -10); ep(resp, 5); ep(resp, 30); ep(resp, -20);
  ep(resp, 0);    ep(resp, 0);   ep(resp, 1000); ep(resp, 5);
  pf32(resp, 5000000.0f);   // amount
  for (int j = 0; j < 6; ++j) ep(resp, 0);  // a..f
  ep(resp, 800);   // up_count
  ep(resp, 200);   // down_count
  for (int j = 0; j < 10; ++j) ep(resp, 0);  // g..p
  // 2 orders: min_point(raw), unknown, vol
  ep(resp, 4000); ep(resp, 0); ep(resp, 100);
  ep(resp, 4010); ep(resp, 1); ep(resp, 50);
  auto ii = deserialize_index_info(resp.data(), resp.size());
  EXPECT_EQ(ii.code, "399001");
  EXPECT_DOUBLE_EQ(ii.close, 4000.0);
  EXPECT_DOUBLE_EQ(ii.pre_close, 3990.0);
  EXPECT_DOUBLE_EQ(ii.open, 4005.0);
  EXPECT_DOUBLE_EQ(ii.high, 4030.0);
  EXPECT_DOUBLE_EQ(ii.low, 3980.0);
  EXPECT_DOUBLE_EQ(ii.diff, 10.0);
  EXPECT_DOUBLE_EQ(ii.amount, 5000000.0);
  EXPECT_EQ(ii.up_count, 800);
  EXPECT_EQ(ii.down_count, 200);
  EXPECT_EQ(ii.orders.size(), 2u);
  EXPECT_EQ(ii.orders[0].price, 4000);   // raw, not accumulated
  EXPECT_EQ(ii.orders[0].vol, 100);
  EXPECT_EQ(ii.orders[1].price, 4010);
  EXPECT_EQ(ii.orders[1].vol, 50);
}

TEST(IndexInfoParser, Empty) {
  IndexInfo ii = deserialize_index_info(nullptr, 0);
  EXPECT_TRUE(ii.code.empty());
}

// ---- 主力异动 0x563 ----
TEST(UnusualParser, Basic) {
  std::vector<uint8_t> resp;
  pu16(resp, 1);              // count=1
  // 32B record: <H6sBBBHH> [15B] + <B3f 13B unpack_type> + 1B unused + <BH 3B time>
  pu16(resp, 1);              // market
  for (char ch : std::string("600000")) resp.push_back(ch);
  resp.push_back(0);          // unknown@8
  resp.push_back(0x03);       // unusual_type@9 = 主力买卖
  resp.push_back(0);          // unknown@10
  pu16(resp, 5);              // index@11
  pu16(resp, 0);              // z@13
  // bytes[15:27] = 13B unpack_by_type: <B3f> v1=0(买入) v2=3.5 v3=2.0 v4=0
  resp.push_back(0x00);       // v1 = 主力买入
  pf32(resp, 3.50f);          // v2
  pf32(resp, 2.00f);          // v3
  pf32(resp, 0.0f);           // v4
  resp.push_back(0);          // unused@28
  resp.push_back(10);         // hour@29
  pu16(resp, 3005);           // minute_sec@30 = 3005 → min=30 sec=05
  auto items = deserialize_unusual(resp.data(), resp.size());
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].code, "600000");
  EXPECT_EQ(items[0].unusual_type, 3);
  EXPECT_EQ(items[0].index, 5);
  EXPECT_EQ(items[0].desc, "主力买入");
  EXPECT_EQ(items[0].value_str, "3.50/2.00");
  EXPECT_EQ(items[0].hour, 10);
  EXPECT_EQ(items[0].minute, 30);
  EXPECT_EQ(items[0].second, 5);
}

TEST(UnusualParser, Empty) {
  EXPECT_TRUE(deserialize_unusual(nullptr, 0).empty());
}

// ---- 请求构造验证 ----
TEST(Serialize, FinanceRequest) {
  auto body = serialize_finance(Market::SH, "600000");
  EXPECT_EQ(body.size(), 9u);
  EXPECT_EQ(body[0], 1);  // type H=1
  EXPECT_EQ(body[2], 1);  // market SH=1
}

TEST(Serialize, HistoryOrdersRequest) {
  auto body = serialize_history_orders(Market::SZ, "000001", 20260701);
  EXPECT_EQ(body.size(), 11u);
  // date LE: 01 07 26 20 → 0x20260701 = b9 d1 34 01 (le)
}

