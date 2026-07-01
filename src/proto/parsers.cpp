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

}  // namespace tdx::proto
