// 扩展行情 parser 实现。逐字移植 opentdx/parser/ex_quotation/server.py（Login）。
#include "tdx/proto/ex_parsers.hpp"

#include <cstdio>

#include "tdx/proto/codec.hpp"  // to_datetime
#include "tdx/proto/sp_codec.hpp"  // combine_to_datetime（ex 历史成交 date+time→epoch）
#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"
#include "tdx/util/time_util.hpp"

namespace tdx::proto {
using tdx::util::push_code;
using tdx::util::push_u16;
using tdx::util::push_u32;

// 扩展行情登录 body：80 字节固定 hex（opentdx ex_quotation/server.py:9-19 逐字节硬编码）。
// 这是预加密/握手包，C++ 不重新生成，逐字节复制。
static const uint8_t kExLoginBody[80] = {
    0xe5, 0xbb, 0x1c, 0x2f, 0xaf, 0xe5, 0x25, 0x94,  // e5bb1c2fafe52594
    0x1f, 0x32, 0xc6, 0xe5, 0xd5, 0x3d, 0xfb, 0x41,  // 1f32c6e5d53dfb41
    0x5b, 0x73, 0x4c, 0xc9, 0xcd, 0xbf, 0x0a, 0xc9,  // 5b734cc9cdbf0ac9
    0x20, 0x21, 0xbf, 0xdd, 0x1e, 0xb0, 0x6d, 0x22,  // 2021bfdd1eb06d22
    0xd0, 0x08, 0x88, 0x4c, 0x16, 0x11, 0xcb, 0x13,  // d008884c1611cb13
    0x78, 0xf6, 0xab, 0xd8, 0x24, 0xd8, 0x99, 0xd2,  // 78f6abd824d899d2
    0x1f, 0x32, 0xc6, 0xe5, 0xd5, 0x3d, 0xfb, 0x41,  // 1f32c6e5d53dfb41
    0x1f, 0x32, 0xc6, 0xe5, 0xd5, 0x3d, 0xfb, 0x41,  // 1f32c6e5d53dfb41
    0xa9, 0x32, 0x5a, 0xc9, 0x35, 0xdc, 0x08, 0x37,  // a9325ac935dc0837
    0x33, 0x5a, 0x16, 0xe4, 0xce, 0x17, 0xc1, 0xbb,  // 335a16e4ce17c1bb
};

std::vector<uint8_t> serialize_ex_login() {
  return std::vector<uint8_t>(kExLoginBody, kExLoginBody + 80);
}

ExLoginResult deserialize_ex_login(const uint8_t* data, std::size_t len) {
  // 对齐 server.py:22 <B52sHBBBBBB21sfBHHH151sBBB52s>，总 299 字节。
  // 偏移（packed）：year@53(H) month@55 day@56 minute@57 hour@58 ms@59 second@60
  //                 server_name@61(21s) u1@82(f) ... desc@93(151s) ... ip@247(52s)
  ExLoginResult r;
  if (len < 299) return r;
  int year = util::rd_u16(data + 53);
  int month = util::rd_u8(data + 55);
  int day = util::rd_u8(data + 56);
  int hour = util::rd_u8(data + 58);
  int minute = util::rd_u8(data + 57);
  int second = util::rd_u8(data + 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                year, month, day, hour, minute, second);
  r.date_time = buf;
  r.server_name = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(data + 61), 21));
  r.desc = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(data + 93), 151));
  r.ip = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(data + 247), 52));
  return r;
}

// ============================ K 线 0x23ff ============================

std::vector<uint8_t> serialize_ex_kline(ExMarket market, std::string_view code, Period period,
                                        uint16_t times, uint32_t start, uint16_t count) {
  // <B9sHHIH>：market(B), code(9s GBK), period(H), times(H), start(I), count(H) = 20B
  std::vector<uint8_t> body;
  body.reserve(20);
  body.push_back(static_cast<uint8_t>(market));
  push_code(body, code, 9);
  push_u16(body, static_cast<uint16_t>(period));
  push_u16(body, times);
  push_u32(body, start);
  push_u16(body, count);
  return body;
}

std::vector<KLine> deserialize_ex_kline(const uint8_t* data, std::size_t len, Period period) {
  // 对齐 ex_quotation/kline.py:14-32。响应 20B 回显 + 32B/根 <IfffffIf>。
  std::vector<KLine> bars;
  if (len < 20) return bars;
  uint16_t count = util::rd_u16(data + 18);  // 请求回显 count@18（B@0 9s@1 H@10 H@12 I@14 H@18）
  uint16_t pv = static_cast<uint16_t>(period);
  bool minute_category = pv < 4 || pv == 7 || pv == 8;
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t off = 20 + static_cast<std::size_t>(i) * 32;
    if (off + 32 > len) break;
    const uint8_t* p = data + off;
    uint32_t date_num = util::rd_u32(p);
    float open = util::rd_f32(p + 4);
    float high = util::rd_f32(p + 8);
    float low = util::rd_f32(p + 12);
    float close = util::rd_f32(p + 16);
    float amount = util::rd_f32(p + 20);
    uint32_t vol = util::rd_u32(p + 24);
    KLine bar;
    bar.datetime = to_datetime(date_num, minute_category);
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.amount = amount;
    bar.volume = static_cast<double>(vol);
    bars.push_back(bar);
  }
  return bars;
}

// ============================ K 线2 0x2489 ============================

std::vector<uint8_t> serialize_ex_kline2(ExMarket market, std::string_view code, Period period,
                                         uint16_t times, uint32_t start, uint32_t count) {
  // <B23sHHII16x>：market(B), code(23s GBK), period(H), times(H), start(I), count(I), 16x = 52B
  std::vector<uint8_t> body;
  body.reserve(52);
  body.push_back(static_cast<uint8_t>(market));
  push_code(body, code, 23);
  push_u16(body, static_cast<uint16_t>(period));
  push_u16(body, times);
  push_u32(body, start);
  push_u32(body, count);
  for (int i = 0; i < 16; ++i) body.push_back(0);
  return body;
}

std::vector<KLine> deserialize_ex_kline2(const uint8_t* data, std::size_t len, Period period) {
  // 对齐 ex_quotation/kline2.py:14-32。响应 42B 回显 + 32B/根 <IfffffII>。
  std::vector<KLine> bars;
  if (len < 42) return bars;
  uint16_t count = util::rd_u16(data + 40);  // 42B 回显末尾 count@40
  uint16_t pv = static_cast<uint16_t>(period);
  bool minute_category = pv < 4 || pv == 7 || pv == 8;
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t off = 42 + static_cast<std::size_t>(i) * 32;
    if (off + 32 > len) break;
    const uint8_t* p = data + off;
    uint32_t date_num = util::rd_u32(p);
    float open = util::rd_f32(p + 4);
    float high = util::rd_f32(p + 8);
    float low = util::rd_f32(p + 12);
    float close = util::rd_f32(p + 16);
    float amount = util::rd_f32(p + 20);
    uint32_t vol = util::rd_u32(p + 24);
    KLine bar;
    bar.datetime = to_datetime(date_num, minute_category);
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.amount = amount;
    bar.volume = static_cast<double>(vol);
    bars.push_back(bar);
  }
  return bars;
}

// ============================ 数量 0x23f0 ============================

std::vector<uint8_t> serialize_ex_count() { return {}; }

uint32_t deserialize_ex_count(const uint8_t* data, std::size_t len) {
  // <11s5I>：market_id(11s) + 5I，count 是第 3 个 I（@11+8=19）
  if (len < 31) return 0;
  return util::rd_u32(data + 19);
}

// ============================ 类别列表 0x23f4 ============================

std::vector<ExCategory> deserialize_ex_category_list(const uint8_t* data, std::size_t len) {
  // 头 <H>count + 64B/条 <B32sB30s>
  std::vector<ExCategory> result;
  if (len < 2) return result;
  uint16_t count = util::rd_u16(data);
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t off = 2 + static_cast<std::size_t>(i) * 64;
    if (off + 64 > len) break;
    const uint8_t* p = data + off;
    ExCategory c;
    c.goods_type = util::rd_u8(p);
    c.name = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 1), 32));
    c.code = util::rd_u8(p + 33);
    c.abbr = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 34), 30));
    result.push_back(std::move(c));
  }
  return result;
}

// ============================ 列表 0x23f5 ============================

std::vector<uint8_t> serialize_ex_list(uint32_t start, uint16_t count) {
  // <IH>：start, count
  std::vector<uint8_t> body;
  push_u32(body, start);
  push_u16(body, count);
  return body;
}

std::vector<ExListItem> deserialize_ex_list(const uint8_t* data, std::size_t len) {
  // 头 <IH>(start@0, count@4) + 64B/条 <BBBH9s26s...>
  std::vector<ExListItem> result;
  if (len < 6) return result;
  uint16_t count = util::rd_u16(data + 4);
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t off = 6 + static_cast<std::size_t>(i) * 64;
    if (off + 64 > len) break;
    const uint8_t* p = data + off;
    ExListItem item;
    item.market = util::rd_u8(p);
    item.category = util::rd_u8(p + 1);
    item.code = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 6), 9));
    item.name = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 15), 26));
    result.push_back(std::move(item));
  }
  return result;
}

// ============================ unpack_futures（报价核心）============================

ExQuote unpack_ex_futures(const uint8_t* data, std::size_t code_len) {
  // 对齐 help.py:233-287。总 291+code_len，4 段。
  ExQuote q;
  q.market = util::rd_u8(data);
  q.code = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(data + 1), code_len));
  std::size_t off = 1 + code_len;
  // 段2 <I5f4If4I> @off~off+60
  q.pre_close = util::rd_f32(data + off + 4);
  q.open = util::rd_f32(data + off + 8);
  q.high = util::rd_f32(data + off + 12);
  q.low = util::rd_f32(data + off + 16);
  q.close = util::rd_f32(data + off + 20);
  q.open_position = util::rd_u32(data + off + 24);
  q.add_position = util::rd_u32(data + off + 28);
  q.vol = util::rd_u32(data + off + 32);
  q.curr_vol = util::rd_u32(data + off + 36);
  q.amount = util::rd_f32(data + off + 40);
  q.hold_position = util::rd_u32(data + off + 56);
  // 段3 盘口 <5f5I5f5I> @off+60~off+140：[0..4]=bid价 [5..9]=bid量 [10..14]=ask价 [15..19]=ask量
  std::size_t hp = off + 60;
  for (int i = 0; i < 5; ++i) {
    q.bid[i].price = util::rd_f32(data + hp + i * 4);
    q.bid[i].vol = util::rd_u32(data + hp + 20 + i * 4);
    q.ask[i].price = util::rd_f32(data + hp + 40 + i * 4);
    q.ask[i].vol = util::rd_u32(data + hp + 60 + i * 4);
  }
  // 段4 <HfIffIIIIf> @off+140：settlement@+2 avg@+10 pre_settlement@+14
  std::size_t mid = off + 140;
  q.settlement = util::rd_f32(data + mid + 2);
  q.avg = util::rd_f32(data + mid + 10);
  q.pre_settlement = util::rd_f32(data + mid + 14);
  // 段5 <12sff12sff25sfIIff24sHB> @off+178：date_raw(I)@+69
  std::size_t tail = off + 178;
  uint32_t date_raw = util::rd_u32(data + tail + 69);
  if (date_raw / 10000 != 0) {
    q.datetime = util::date_to_epoch(date_raw / 10000, date_raw % 10000 / 100, date_raw % 100);
  } else {
    q.datetime = util::date_to_epoch(1900, 1, 1);
  }
  return q;
}

std::vector<uint8_t> serialize_ex_quotes(const std::vector<std::pair<ExMarket, std::string>>& codes) {
  // <B7xH>(5,0×7,count) + N×<B23s>
  std::vector<uint8_t> body;
  body.push_back(5);
  for (int i = 0; i < 7; ++i) body.push_back(0);
  push_u16(body, static_cast<uint16_t>(codes.size()));
  for (const auto& kv : codes) {
    body.push_back(static_cast<uint8_t>(kv.first));
    push_code(body, kv.second, 23);
  }
  return body;
}

std::vector<ExQuote> deserialize_ex_quotes(const uint8_t* data, std::size_t len) {
  // <IIH> @0..10, count@8；每条 314B（code_len=23）
  std::vector<ExQuote> result;
  if (len < 10) return result;
  uint16_t count = util::rd_u16(data + 8);
  std::size_t pos = 10;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 314 > len) break;
    result.push_back(unpack_ex_futures(data + pos, 23));
    pos += 314;
  }
  return result;
}

// ============================ 历史成交 0x2412 ============================

std::vector<uint8_t> serialize_ex_history_txn(ExMarket market, std::string_view code, int ymd) {
  // <IB43sH>：date(I), market(B), code(43s), 0x78(H)
  std::vector<uint8_t> body;
  push_u32(body, static_cast<uint32_t>(ymd));
  body.push_back(static_cast<uint8_t>(market));
  push_code(body, code, 43);
  push_u16(body, 0x78);
  return body;
}

std::vector<Transaction> deserialize_ex_history_txn(const uint8_t* data, std::size_t len) {
  // <B39sIfIIH> @0..58, count@56；每笔 16B <HIIIH>
  std::vector<Transaction> result;
  if (len < 58) return result;
  uint16_t count = util::rd_u16(data + 56);
  uint32_t date_ymd = util::rd_u32(data + 40);  // head date @40（opentdx <B39sI...>）
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t off = 58 + static_cast<std::size_t>(i) * 16;
    if (off + 16 > len) break;
    uint16_t minutes = util::rd_u16(data + off);
    uint32_t price = util::rd_u32(data + off + 2);
    uint32_t vol = util::rd_u32(data + off + 6);
    uint16_t buy_sell = util::rd_u16(data + off + 14);
    Transaction t;
    // opentdx 仅给 time(minutes)，此处用 head date 组合成真 epoch（统一 datetime=epoch 语义）
    t.datetime = combine_to_datetime(date_ymd, static_cast<int>(minutes) * 60, false);
    t.price = static_cast<double>(price);
    t.volume = static_cast<double>(vol);
    t.buy_sell = buy_sell == 0 ? BuySell::Buy
                 : (buy_sell == 1 ? BuySell::Sell : BuySell::Neutral);
    result.push_back(t);
  }
  return result;
}

}  // namespace tdx::proto
