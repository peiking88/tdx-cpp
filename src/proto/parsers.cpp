// 通达信行情 parser 实现（kline/tick/transaction/list/count/login/heartbeat）。
// 逐字移植 opentdx/parser/quotation/* + utils/help.py。quotes_detail 见 parsers_quotes.cpp。
#include "tdx/proto/parsers.hpp"

#include <cstring>
#include <ctime>

#include "tdx/data/scaling.hpp"
#include "tdx/proto/codec.hpp"        // get_price / to_datetime
#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"

namespace tdx::proto {
using tdx::util::push_u16;
using tdx::util::push_u32;

// push_code from tdx/util/byte_order.hpp (width=6 for GBK code)
using tdx::util::push_code;

// ============================ K 线 0x523 ============================

std::vector<uint8_t> serialize_kline(Market market, std::string_view code, Period period,
                                     uint16_t times, uint16_t start, uint16_t count,
                                     Adjust adjust) {
  // <H6sHHHHH8s>：market, code(6), period, times, start, count, adjust, 8 字节占位
  std::vector<uint8_t> body;
  body.reserve(26);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u16(body, static_cast<uint16_t>(period));
  push_u16(body, times);
  push_u16(body, start);
  push_u16(body, count);
  push_u16(body, static_cast<uint16_t>(adjust));
  for (int i = 0; i < 8; ++i) body.push_back(0);
  return body;
}

std::vector<KLine> deserialize_kline(const uint8_t* data, std::size_t len, Period period) {
  // 对齐 opentdx kline.py:17-89。
  std::vector<KLine> bars;
  if (len < 2) return bars;
  uint16_t count = util::rd_u16(data);
  std::size_t pos = 2;
  uint16_t pv = static_cast<uint16_t>(period);
  bool minute_category = pv < 4 || pv == 7 || pv == 8;
  const std::size_t MIN_BAR = 13;

  for (uint16_t i = 0; i < count; ++i) {
    if (pos + MIN_BAR > len) break;
    uint32_t date_num = util::rd_u32(data + pos);
    pos += 4;
    int64_t datetime = to_datetime(date_num, minute_category);

    // OHLC 顺序：open → close → high → low（反直觉，D3）。每步前检查剩余空间。
    if (pos + 9 > len) break;
    auto open = get_price(data, len, pos); pos = open.new_pos;
    if (pos + 9 > len) break;
    auto close = get_price(data, len, pos); pos = close.new_pos;
    if (pos + 9 > len) break;
    auto high = get_price(data, len, pos); pos = high.new_pos;
    if (pos + 9 > len) break;
    auto low = get_price(data, len, pos); pos = low.new_pos;

    if (pos + 8 > len) break;
    float vol = util::rd_f32(data + pos);
    float amount = util::rd_f32(data + pos + 4);
    pos += 8;

    // upCount/downCount 智能检测（D6）：试探后 4 字节是计数还是下一根日期
    int32_t up_count = 0, down_count = 0;
    if (pos + 4 <= len) {
      uint32_t try_date = util::rd_u32(data + pos);
      bool is_count = false;
      if (minute_category) {
        int64_t try_dt = to_datetime(try_date, minute_category);
        if (try_dt <= datetime) is_count = true;
      } else {
        uint32_t y = try_date / 10000, m = try_date % 10000 / 100, d = try_date % 100;
        if (y < 1990 || m < 1 || m > 12 || d < 1 || d > 31) {
          is_count = true;
        } else {
          int64_t try_dt = to_datetime(try_date, minute_category);
          if (try_dt <= datetime) is_count = true;
        }
      }
      if (is_count) {
        up_count = util::rd_u16(data + pos);
        down_count = util::rd_u16(data + pos + 2);
        pos += 4;
      }
    }

    KLine bar;
    bar.datetime = datetime;
    auto s = tdx::data::GetScaling(tdx::data::DataSource::NetKlineStd);
    bar.open = static_cast<double>(open.value) * s.ohlc;
    bar.close = static_cast<double>(close.value) * s.ohlc;
    bar.high = static_cast<double>(high.value) * s.ohlc;
    bar.low = static_cast<double>(low.value) * s.ohlc;
    bar.volume = static_cast<double>(vol);
    bar.amount = static_cast<double>(amount);
    bar.up_count = up_count;
    bar.down_count = down_count;
    bars.push_back(std::move(bar));
  }
  return bars;
}

// ============================ 分时 0x537 ============================

std::vector<uint8_t> serialize_tick(Market market, std::string_view code,
                                    uint16_t start, uint16_t count) {
  // <H6sHH>：market, code(6), start, count
  std::vector<uint8_t> body;
  body.reserve(12);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u16(body, start);
  push_u16(body, count);
  return body;
}

std::vector<Tick> deserialize_tick(const uint8_t* data, std::size_t len) {
  // 对齐 opentdx tick_chart.py:15-36。双增量：输出 = start_X + X，首条后锁定 start_X。
  std::vector<Tick> result;
  if (len < 4) return result;
  uint16_t num = util::rd_u16(data);  // 前 4 字节两个 H，只用第一个
  std::size_t pos = 4;
  int64_t start_price = 0, start_avg = 0;
  for (uint16_t i = 0; i < num; ++i) {
    auto price = get_price(data, len, pos); pos = price.new_pos;
    auto avg = get_price(data, len, pos); pos = avg.new_pos;
    auto vol = get_price(data, len, pos); pos = vol.new_pos;
    Tick t;
    auto s = tdx::data::GetScaling(tdx::data::DataSource::NetTick);
    t.price = static_cast<double>(start_price + price.value) * s.price;
    t.avg   = static_cast<double>(start_avg + avg.value) * s.avg;
    t.volume = static_cast<double>(vol.value) * s.volume;
    result.push_back(t);
    if (start_price == 0) start_price = price.value;
    if (start_avg == 0) start_avg = avg.value;
  }
  return result;
}

// ============================ 逐笔 0xfc5 ============================

std::vector<uint8_t> serialize_transaction(Market market, std::string_view code,
                                           uint16_t start, uint16_t count) {
  std::vector<uint8_t> body;
  body.reserve(12);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u16(body, start);
  push_u16(body, count);
  return body;
}

std::vector<Transaction> deserialize_transaction(const uint8_t* data, std::size_t len) {
  // 对齐 opentdx transaction.py:16-41。price 增量累加（last_price += price）。
  std::vector<Transaction> result;
  if (len < 2) return result;
  uint16_t count = util::rd_u16(data);
  std::size_t pos = 2;
  int64_t last_price = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 2 > len) break;
    uint16_t minutes = util::rd_u16(data + pos);
    pos += 2;
    auto price = get_price(data, len, pos); pos = price.new_pos;
    auto vol = get_price(data, len, pos); pos = vol.new_pos;
    auto trans = get_price(data, len, pos); pos = trans.new_pos;
    auto buy_sell = get_price(data, len, pos); pos = buy_sell.new_pos;
    auto unknown = get_price(data, len, pos); pos = unknown.new_pos;
    (void)unknown;
    last_price += price.value;
    int hour = minutes / 60 % 24;
    int minute = minutes % 60;
    // 用当日 0 点 + minutes 构造 epoch（minutes 为当日分钟数）
    // opentdx 用 time(h, m) 无日期；这里锚定到 epoch 的当日（UTC 基准的整数小时分钟）
    Transaction txn;
    txn.datetime = static_cast<int64_t>(hour) * 3600 + minute * 60;  // 当日秒偏移（下游可叠加日期）
    txn.price = static_cast<double>(last_price) * tdx::data::GetScaling(tdx::data::DataSource::NetTransaction).price;
    txn.volume = vol.value;
    txn.trans_id = trans.value;
    switch (buy_sell.value) {
      case 0: txn.buy_sell = BuySell::Buy; break;
      case 1: txn.buy_sell = BuySell::Sell; break;
      default: txn.buy_sell = BuySell::Neutral; break;
    }
    result.push_back(txn);
  }
  return result;
}

// ============================ 列表 0x44d ============================

std::vector<uint8_t> serialize_list(Market market, uint16_t start, uint16_t count) {
  // <H3I>：market, start, count, 0
  std::vector<uint8_t> body;
  body.reserve(14);
  push_u16(body, static_cast<uint16_t>(market));
  push_u32(body, start);
  push_u32(body, count);
  push_u32(body, 0);
  return body;
}

std::vector<Stock> deserialize_list(const uint8_t* data, std::size_t len) {
  // 对齐 opentdx list.py:14-31。固定 37 字节/条 <6sH16sfBfHH>。
  std::vector<Stock> result;
  if (len < 2) return result;
  uint16_t count = util::rd_u16(data);
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t pos = 2 + static_cast<std::size_t>(i) * 37;
    if (pos + 37 > len) break;
    const uint8_t* p = data + pos;
    // code(6) vol(2) name(16) unknown1(f4) decimal_point(B1) pre_close(f4) unknown2(H2) unknown3(H2)
    std::string code_raw(reinterpret_cast<const char*>(p), 6);
    // vol @ offset 6（H）
    // name @ offset 8（16s）
    std::string name_raw(reinterpret_cast<const char*>(p + 8), 16);
    // unknown1 @ offset 24（f）
    uint8_t decimal_point = util::rd_u8(p + 28);
    float pre_close = util::rd_f32(p + 29);
    // unknown2 @ 33, unknown3 @ 35

    Stock s;
    s.code = util::trim_null(code_raw);
    s.name = util::gbk_to_utf8(util::trim_null(name_raw));
    s.market = 0;  // 响应不含市场，由调用方（StdQuotes）按请求市场填入
    s.pre_close = pre_close;
    s.decimal_point = decimal_point;
    result.push_back(std::move(s));
  }
  return result;
}

// ============================ 数量 0x44e ============================

std::vector<uint8_t> serialize_count(Market market) {
  // <HI>：market, today(YYYYMMDD)。today 用当前日期。
  std::vector<uint8_t> body;
  body.reserve(6);
  push_u16(body, static_cast<uint16_t>(market));
  // today：用 std::time 取当前年月日拼装
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  uint32_t today = static_cast<uint32_t>((tm.tm_year + 1900) * 10000 +
                                         (tm.tm_mon + 1) * 100 + tm.tm_mday);
  push_u32(body, today);
  return body;
}

uint16_t deserialize_count(const uint8_t* data, std::size_t len) {
  if (len < 2) return 0;
  return util::rd_u16(data);
}

// ============================ 登录 0x0d / 心跳 0x04 ============================

std::vector<uint8_t> serialize_login() {
  // <B=1>（opentdx server.py:91-92）
  return {0x01};
}

std::vector<uint8_t> serialize_heartbeat() {
  return {};  // 心跳请求体空
}

// ============================ 财务 0x10 ============================

std::vector<uint8_t> serialize_finance(Market market, std::string_view code) {
  // <HB6s>：type(1), market, code(6)
  std::vector<uint8_t> body;
  body.reserve(9);
  push_u16(body, 1);
  body.push_back(static_cast<uint8_t>(market));
  push_code(body, code, 6);
  return body;
}

Finance deserialize_finance(const uint8_t* data, std::size_t len) {
  Finance f{};
  // <HB6sfHHIIffffffffffffffffffffffffffffff> = 2+1+6+4+2+2+4+4+29×4 = 141B
  if (len < 141) return f;
  f.liutongguben = util::rd_f32(data + 9);                        // +9 = H(2)+B(1)+6s(6)
  f.province     = util::rd_u16(data + 13);
  f.industry     = util::rd_u16(data + 15);
  f.updated_date = util::rd_u32(data + 17);
  f.ipo_date     = util::rd_u32(data + 21);
  const uint8_t* p = data + 25;
  f.zongguben             = util::rd_f32(p);      p += 4;
  f.guojiagu              = util::rd_f32(p);      p += 4;
  f.faqirenfarengu        = util::rd_f32(p);      p += 4;
  f.farengu               = util::rd_f32(p);      p += 4;
  f.bgu                   = util::rd_f32(p);      p += 4;
  f.hgu                   = util::rd_f32(p);      p += 4;
  f.meigushouyi           = util::rd_f32(p);      p += 4;
  f.zichanzongji          = util::rd_f32(p);      p += 4;
  f.liudongzichanzongji   = util::rd_f32(p);      p += 4;
  f.gudingzichanjine      = util::rd_f32(p);      p += 4;
  f.wuxingzichan          = util::rd_f32(p);      p += 4;
  f.gudongrenshu          = util::rd_f32(p);      p += 4;
  f.liudongfuzhaiheji     = util::rd_f32(p);      p += 4;
  f.changqifuzhai         = util::rd_f32(p);      p += 4;
  f.zibengongjijin        = util::rd_f32(p);      p += 4;
  f.guimuquanyineji       = util::rd_f32(p);      p += 4;
  f.yinyezongshouru       = util::rd_f32(p);      p += 4;
  f.yinyechengben         = util::rd_f32(p);      p += 4;
  f.yingshouzhanngkuan    = util::rd_f32(p);      p += 4;
  f.yinyelirun            = util::rd_f32(p);      p += 4;
  f.touzishouyi           = util::rd_f32(p);      p += 4;
  f.jingyinxianjinliujine = util::rd_f32(p);      p += 4;
  f.zongxianjinliu        = util::rd_f32(p);      p += 4;
  f.cunhuo                = util::rd_f32(p);      p += 4;
  f.lirunzonge            = util::rd_f32(p);      p += 4;
  f.shuihoulirun          = util::rd_f32(p);      p += 4;
  f.guimujinlirun         = util::rd_f32(p);      p += 4;
  f.weifenlirun           = util::rd_f32(p);      p += 4;
  f.meigujinzichan        = util::rd_f32(p);
  std::string code_raw(reinterpret_cast<const char*>(data + 3), 6);
  f.code = util::trim_null(code_raw);
  return f;
}

// ============================ F10 分类 0x2cf ============================

std::vector<uint8_t> serialize_f10_category(Market market, std::string_view code) {
  std::vector<uint8_t> body;
  body.reserve(12);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u32(body, 0);
  return body;
}

std::vector<F10Category> deserialize_f10_category(const uint8_t* data, std::size_t len) {
  std::vector<F10Category> result;
  if (len < 2) return result;
  uint16_t count = util::rd_u16(data);
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t pos = 2 + static_cast<std::size_t>(i) * 152;
    if (pos + 152 > len) break;
    F10Category c;
    std::string name_raw(reinterpret_cast<const char*>(data + pos), 64);
    std::string fn_raw(reinterpret_cast<const char*>(data + pos + 64), 80);
    c.name = util::gbk_to_utf8(util::trim_null(name_raw));
    c.filename = util::gbk_to_utf8(util::trim_null(fn_raw));
    c.start = util::rd_u32(data + pos + 144);
    c.length = util::rd_u32(data + pos + 148);
    result.push_back(std::move(c));
  }
  return result;
}

// ============================ F10 内容 0x2d0 ============================

std::vector<uint8_t> serialize_f10_content(Market market, std::string_view code,
                                            std::string_view filename,
                                            uint32_t start, uint32_t length) {
  std::vector<uint8_t> body;
  body.reserve(2 + 6 + 2 + 80 + 4 + 4 + 4);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u16(body, 0);
  // filename padded to 80 bytes GBK
  // ponytail: use ASCII/UTF8 filename, pad with zero
  for (size_t j = 0; j < 80; ++j) {
    body.push_back(j < filename.size() ? filename[j] : 0);
  }
  push_u32(body, start);
  push_u32(body, length);
  push_u32(body, 0);
  return body;
}

F10Content deserialize_f10_content(const uint8_t* data, std::size_t len) {
  F10Content c{};
  if (len < 12) return c;
  c.market = util::rd_u16(data);
  std::string code_raw(reinterpret_cast<const char*>(data + 2), 6);
  c.code = util::trim_null(code_raw);
  c.category = util::rd_u16(data + 8);
  c.length = util::rd_u16(data + 10);   // 对齐 Python <H6sHH>：length 是 H(u16)，非 u32
  if (len >= static_cast<std::size_t>(12 + c.length)) {
    c.content = util::gbk_to_utf8(std::string(reinterpret_cast<const char*>(data + 12), c.length));
  }
  return c;
}

// ============================ 历史委托 0xfb4 ============================

std::vector<uint8_t> serialize_history_orders(Market market, std::string_view code,
                                               uint32_t date_yyyymmdd) {
  std::vector<uint8_t> body;
  body.reserve(4 + 1 + 6);
  push_u32(body, date_yyyymmdd);
  body.push_back(static_cast<uint8_t>(market));
  push_code(body, code, 6);
  return body;
}

std::vector<HistoryOrder> deserialize_history_orders(const uint8_t* data, std::size_t len) {
  std::vector<HistoryOrder> result;
  if (len < 6) return result;
  uint16_t count = util::rd_u16(data);
  std::size_t pos = 6;
  int64_t last_price = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos >= len) break;                                    // 对齐 deserialize_history_transaction 的 guard
    auto price = get_price(data, len, pos); pos = price.new_pos;
    if (pos >= len) break;
    auto unknown = get_price(data, len, pos); pos = unknown.new_pos;
    if (pos >= len) break;
    auto vol = get_price(data, len, pos); pos = vol.new_pos;
    last_price += price.value;
    HistoryOrder o;
    o.price = static_cast<double>(last_price) * tdx::data::GetScaling(tdx::data::DataSource::NetTransaction).price;
    o.unknown = unknown.value;
    o.vol = vol.value;
    result.push_back(o);
  }
  return result;
}

// ============================ 历史逐笔 0xfb5 ============================

std::vector<uint8_t> serialize_history_transaction(Market market, std::string_view code,
                                                    uint32_t date_yyyymmdd,
                                                    uint16_t start, uint16_t count) {
  std::vector<uint8_t> body;
  body.reserve(4 + 2 + 6 + 2 + 2);
  push_u32(body, date_yyyymmdd);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u16(body, start);
  push_u16(body, count);
  return body;
}

std::vector<HistoryTransaction> deserialize_history_transaction(const uint8_t* data, std::size_t len) {
  std::vector<HistoryTransaction> result;
  if (len < 6) return result;
  uint16_t count = util::rd_u16(data);
  std::size_t pos = 6;
  int64_t last_price = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 2 > len) break;
    uint16_t minutes = util::rd_u16(data + pos); pos += 2;
    auto price = get_price(data, len, pos); pos = price.new_pos;
    auto vol = get_price(data, len, pos); pos = vol.new_pos;
    auto buy_sell = get_price(data, len, pos); pos = buy_sell.new_pos;
    auto unknown = get_price(data, len, pos); pos = unknown.new_pos;
    (void)unknown;
    last_price += price.value;
    HistoryTransaction t;
    t.minutes = minutes;
    t.price = static_cast<double>(last_price) * tdx::data::GetScaling(tdx::data::DataSource::NetTransaction).price;
    t.vol = vol.value;
    t.buy_sell = static_cast<int>(buy_sell.value);
    result.push_back(t);
  }
  return result;
}

// ============================ 成交量分布 0x51a ============================

std::vector<uint8_t> serialize_volume_profile(Market market, std::string_view code) {
  std::vector<uint8_t> body;
  body.reserve(2 + 6);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  return body;
}

VolProfile deserialize_volume_profile(const uint8_t* data, std::size_t len) {
  VolProfile vp{};
  if (len < 11) return vp;
  uint16_t count = util::rd_u16(data);
  vp.market = data[2];
  std::string code_raw(reinterpret_cast<const char*>(data + 3), 6);
  vp.code = util::trim_null(code_raw);
  std::size_t pos = 11;
  auto s = tdx::data::GetScaling(tdx::data::DataSource::NetQuotes);
  auto price = get_price(data, len, pos); pos = price.new_pos;
  int64_t base = price.value;
  auto pre_close_p = get_price(data, len, pos); pos = pre_close_p.new_pos;
  vp.pre_close = static_cast<double>(base + pre_close_p.value) * s.pre_close;
  auto open_p = get_price(data, len, pos); pos = open_p.new_pos;
  vp.open = static_cast<double>(base + open_p.value) * s.ohlc;
  auto high_p = get_price(data, len, pos); pos = high_p.new_pos;
  vp.high = static_cast<double>(base + high_p.value) * s.ohlc;
  auto low_p = get_price(data, len, pos); pos = low_p.new_pos;
  vp.low = static_cast<double>(base + low_p.value) * s.ohlc;
  auto server_time = get_price(data, len, pos); pos = server_time.new_pos;
  (void)server_time;
  auto neg_price = get_price(data, len, pos); pos = neg_price.new_pos;
  (void)neg_price;
  auto vol_p = get_price(data, len, pos); pos = vol_p.new_pos;
  vp.vol = static_cast<double>(vol_p.value);
  auto cur_vol = get_price(data, len, pos); pos = cur_vol.new_pos;
  (void)cur_vol;
  if (pos + 4 > len) return vp;
  vp.amount = util::rd_f32(data + pos) * s.amount; pos += 4;
  // skip s_vol, b_vol, s_amount, open_amount
  auto sv = get_price(data, len, pos); pos = sv.new_pos;
  auto bv = get_price(data, len, pos); pos = bv.new_pos;
  auto sa = get_price(data, len, pos); pos = sa.new_pos;
  auto oa = get_price(data, len, pos); pos = oa.new_pos;
  (void)sv; (void)bv; (void)sa; (void)oa;
  // 三档盘口
  for (int j = 0; j < 3; ++j) {
    auto bid = get_price(data, len, pos); pos = bid.new_pos;
    auto ask = get_price(data, len, pos); pos = ask.new_pos;
    auto bid_vol = get_price(data, len, pos); pos = bid_vol.new_pos;
    auto ask_vol = get_price(data, len, pos); pos = ask_vol.new_pos;
    QuoteLevel bl;
    bl.price = static_cast<double>(base + bid.value) * s.bid_ask;
    bl.vol = static_cast<double>(bid_vol.value);
    vp.handicap_bid.push_back(bl);
    QuoteLevel al;
    al.price = static_cast<double>(base + ask.value) * s.bid_ask;
    al.vol = static_cast<double>(ask_vol.value);
    vp.handicap_ask.push_back(al);
  }
  if (pos + 2 > len) return vp;
  pos += 2;  // unknown H
  vp.price = static_cast<double>(base) * s.price;
  int64_t start_price = 0;
  for (uint16_t i = 0; i < count; ++i) {
    auto p = get_price(data, len, pos); pos = p.new_pos;
    auto v = get_price(data, len, pos); pos = v.new_pos;
    auto buy = get_price(data, len, pos); pos = buy.new_pos;
    auto sell = get_price(data, len, pos); pos = sell.new_pos;
    start_price += p.value;
    VolProfileLevel lvl;
    lvl.price = static_cast<double>(start_price);
    lvl.vol = v.value;
    lvl.buy = buy.value;
    lvl.sell = sell.value;
    vp.levels.push_back(lvl);
  }
  return vp;
}

// ============================ 指数信息 0x51d ============================

std::vector<uint8_t> serialize_index_info(Market market, std::string_view code) {
  std::vector<uint8_t> body;
  body.reserve(2 + 6 + 4);
  push_u16(body, static_cast<uint16_t>(market));
  push_code(body, code, 6);
  push_u32(body, 0);
  return body;
}

IndexInfo deserialize_index_info(const uint8_t* data, std::size_t len) {
  IndexInfo ii{};
  if (len < 13) return ii;
  uint32_t count = util::rd_u32(data);         // data[0:4] = count(orders) I
  ii.market = static_cast<int>(data[4]);
  std::string code_raw(reinterpret_cast<const char*>(data + 5), 6);
  ii.code = util::trim_null(code_raw);
  std::size_t pos = 13;
  auto close_p = get_price(data, len, pos); pos = close_p.new_pos;
  int64_t base = close_p.value;
  ii.close = static_cast<double>(base);
  auto pre_close_diff = get_price(data, len, pos); pos = pre_close_diff.new_pos;
  ii.pre_close = static_cast<double>(base + pre_close_diff.value);
  ii.diff = static_cast<double>(-pre_close_diff.value);
  auto open_diff = get_price(data, len, pos); pos = open_diff.new_pos;
  ii.open = static_cast<double>(base + open_diff.value);
  auto high_diff = get_price(data, len, pos); pos = high_diff.new_pos;
  ii.high = static_cast<double>(base + high_diff.value);
  auto low_diff = get_price(data, len, pos); pos = low_diff.new_pos;
  ii.low = static_cast<double>(base + low_diff.value);
  { auto t = get_price(data, len, pos); pos = t.new_pos; }     // server_time
  { auto t = get_price(data, len, pos); pos = t.new_pos; }     // after_hour
  auto vol_p = get_price(data, len, pos); pos = vol_p.new_pos;
  ii.vol = static_cast<double>(vol_p.value);
  { auto t = get_price(data, len, pos); pos = t.new_pos; }     // cur_vol
  if (pos + 4 > len) return ii;
  ii.amount = util::rd_f32(data + pos); pos += 4;
  // skip a, b, open_amount, d, e, f (6 fields)
  for (int j = 0; j < 6; ++j) { auto t = get_price(data, len, pos); pos = t.new_pos; }
  auto up = get_price(data, len, pos); ii.up_count = up.value; pos = up.new_pos;
  auto down = get_price(data, len, pos); ii.down_count = down.value; pos = down.new_pos;
  // skip g..p (10 fields) (index_info.py:60-67)
  for (int j = 0; j < 10; ++j) { auto t = get_price(data, len, pos); pos = t.new_pos; }
  // parse count orders — min_point NOT accumulated (index_info.py:73-76)
  for (uint32_t i = 0; i < count; ++i) {
    auto mp = get_price(data, len, pos); pos = mp.new_pos;
    auto mu = get_price(data, len, pos); pos = mu.new_pos;
    auto mv = get_price(data, len, pos); pos = mv.new_pos;
    ii.orders.push_back(IndexOrder{mp.value, mu.value, mv.value});
  }
  return ii;
}

// ============================ 主力异动 0x563 ============================

std::vector<uint8_t> serialize_unusual(Market market, uint16_t start, uint16_t count) {
  std::vector<uint8_t> body;
  body.reserve(2 + 4 + 4);
  push_u16(body, static_cast<uint16_t>(market));
  push_u32(body, start);
  push_u32(body, count);
  return body;
}

// 对齐 Python unpack_by_type(help.py): 按 unusual_type 从 <B3f 13 字节中解析 desc + val。
static void UnpackByType(int utype, const uint8_t* d, std::string& desc, std::string& val) {
  uint8_t v1 = d[0];
  float v2 = util::rd_f32(d + 1);
  float v3 = util::rd_f32(d + 5);
  float v4 = util::rd_f32(d + 9);
  char b[64];
  switch (utype) {
  case 0x03:
    desc = v1 == 0 ? "主力买入" : "主力卖出";
    std::snprintf(b, sizeof(b), "%.2f/%.2f", v2, v3); val = b; break;
  case 0x04: desc = "加速拉升"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x05: desc = "加速下跌"; break;
  case 0x06: desc = "低位反弹"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x07: desc = "高位回落"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x08: desc = "撑杆跳高"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x09: desc = "平台跳水"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x0a:
    desc = v2 < 0 ? "单笔冲跌" : "单笔冲涨";
    std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x0b: {
    const char* dir = v3 == 0 ? "平" : (v3 < 0 ? "跌" : "涨");
    char tmp[32]; int n = std::snprintf(tmp, sizeof(tmp), "区间放量%s", dir);
    desc.assign(tmp, n);
    if (v3 == 0) std::snprintf(b, sizeof(b), "%.1f倍", v2);
    else std::snprintf(b, sizeof(b), "%.1f倍%.2f%%", v2, v3 * 100);
    val = b; break;
  }
  case 0x0c: desc = "区间缩量"; break;
  case 0x10: desc = "大单托盘"; std::snprintf(b, sizeof(b), "%.2f/%.2f", v4, v3); val = b; break;
  case 0x11: desc = "大单压盘"; std::snprintf(b, sizeof(b), "%.2f/%.2f", v2, v3); val = b; break;
  case 0x12: desc = "大单锁盘"; break;
  case 0x13: desc = "竞价试买"; std::snprintf(b, sizeof(b), "%.2f/%.2f", v2, v3); val = b; break;
  case 0x14: {
    uint8_t st = d[1];  // sub_type
    const char* dir = v1 == 0 ? "涨" : "跌";
    if (st == 1)      { char t[32]; int n = std::snprintf(t, sizeof(t), "逼近%s停", dir); desc.assign(t, n); }
    else if (st == 2) { char t[32]; int n = std::snprintf(t, sizeof(t), "封%s停板", dir); desc.assign(t, n); }
    else if (st == 4) { char t[32]; int n = std::snprintf(t, sizeof(t), "封%s大减", dir); desc.assign(t, n); }
    else if (st == 5) { char t[32]; int n = std::snprintf(t, sizeof(t), "打开%s停", dir); desc.assign(t, n); }
    float sv2 = util::rd_f32(d + 2);
    float sv3 = util::rd_f32(d + 6);
    std::snprintf(b, sizeof(b), "%.2f/%.2f", sv2, sv3); val = b; break;
  }
  case 0x15:
    if (v1 == 0) desc = "尾盘??";
    else if (v1 == 1) desc = "尾盘对倒";
    else if (v1 == 2) desc = "尾盘拉升";
    else desc = "尾盘打压";
    std::snprintf(b, sizeof(b), "%.2f%%/%.2f", v2 * 100, v3); val = b; break;
  case 0x16:
    desc = v2 < 0 ? "盘中弱势" : "盘中强势";
    std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x1d: desc = "急速拉升"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  case 0x1e: desc = "急速下跌"; std::snprintf(b, sizeof(b), "%.2f%%", v2 * 100); val = b; break;
  default: desc = std::to_string(utype); break;
  }
}

std::vector<UnusualItem> deserialize_unusual(const uint8_t* data, std::size_t len) {
  std::vector<UnusualItem> result;
  if (len < 2) return result;
  uint16_t count = util::rd_u16(data);
  constexpr std::size_t REC = 32;
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t pos = 2 + static_cast<std::size_t>(i) * REC;
    if (pos + REC > len) break;
    UnusualItem u;
    u.market = util::rd_u16(data + pos);
    std::string c(reinterpret_cast<const char*>(data + pos + 2), 6);
    u.code = util::trim_null(c);
    // <H6sBBBHH>: market@0 code@2 unknown@8 unusual_type@9 unknown@10 index@11 z@13 → 15B
    u.unusual_type = data[pos + 9];
    u.index = util::rd_u16(data + pos + 11);
    // bytes[17:29] = 13B unpack_by_type area (Python data[32i+17:32i+30])
    UnpackByType(u.unusual_type, data + pos + 15, u.desc, u.value_str);
    // simplifed value for sorting/display
    u.value = u.desc.empty() ? 0.0 : static_cast<double>(util::rd_f32(data + pos + 16)); // v2 as value
    u.hour = data[pos + 29];
    uint16_t ms = util::rd_u16(data + pos + 30);
    u.minute = ms / 100;
    u.second = ms % 100;
    result.push_back(u);
  }
  return result;
}

}  // namespace tdx::proto
