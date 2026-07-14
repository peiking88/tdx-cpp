// 五档报价 parser 0x53e（最复杂：price 基准 + OHLC/五档相对增量）。
// 逐字移植 opentdx/parser/quotation/quotes_detail.py:22-97。
#include "tdx/proto/parsers.hpp"

#include "tdx/data/scaling.hpp"
#include "tdx/proto/codec.hpp"
#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"

namespace tdx::proto {
using tdx::util::push_code;
using tdx::util::push_u16;

std::vector<uint8_t> serialize_quotes_detail(const std::vector<QuoteReq>& stocks) {
  // 请求体 <H6sH>(5,'',count) + N×<B6s>(market,code)
  std::vector<uint8_t> body;
  body.reserve(10 + 7 * stocks.size());
  push_u16(body, 5);  // magic 常量 5（quotes_detail.py:16）
  for (int i = 0; i < 6; ++i) body.push_back(0);  // 6s 空
  push_u16(body, static_cast<uint16_t>(stocks.size()));
  for (const auto& s : stocks) {
    body.push_back(static_cast<uint8_t>(s.market));
    push_code(body, s.code, 6);
  }
  return body;
}

std::vector<Quote> deserialize_quotes_detail(const uint8_t* data, std::size_t len) {
  std::vector<Quote> result;
  if (len < 4) return result;
  uint16_t count = util::rd_u16(data + 2);  // 头部 <HH>，用第二个 H（quotes_detail.py:23）
  std::size_t pos = 4;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 9 > len) break;
    // <B6sH>：market(1) code(6) active1(2)
    std::string code_raw(reinterpret_cast<const char*>(data + pos + 1), 6);
    pos += 9;

    // price 是基准，后续 OHLC/pre_close/五档 bid/ask 都相对它增量
    auto price = get_price(data, len, pos); pos = price.new_pos;
    auto pre_close = get_price(data, len, pos); pos = pre_close.new_pos;
    auto open = get_price(data, len, pos); pos = open.new_pos;
    auto high = get_price(data, len, pos); pos = high.new_pos;
    auto low = get_price(data, len, pos); pos = low.new_pos;
    auto server_time = get_price(data, len, pos); pos = server_time.new_pos;
    auto neg_price = get_price(data, len, pos); pos = neg_price.new_pos;
    auto vol = get_price(data, len, pos); pos = vol.new_pos;
    auto cur_vol = get_price(data, len, pos); pos = cur_vol.new_pos;

    if (pos + 4 > len) break;
    float amount = util::rd_f32(data + pos);  // amount 是唯一 float32（非 get_price）
    pos += 4;

    auto s_vol = get_price(data, len, pos); pos = s_vol.new_pos;
    auto b_vol = get_price(data, len, pos); pos = b_vol.new_pos;
    auto s_amount = get_price(data, len, pos); pos = s_amount.new_pos;
    auto open_amount = get_price(data, len, pos); pos = open_amount.new_pos;
    (void)neg_price; (void)cur_vol; (void)s_vol; (void)b_vol;
    (void)s_amount; (void)open_amount;

    Quote q;
    q.datetime = format_time_to_epoch(server_time.value);  // realtime quote: HHMMSSmm → 当日 epoch
    int64_t base = price.value;
    q.code = util::trim_null(code_raw);  // 上移：供 ClassifySecurity 区分基金/个股缩放
    auto s = tdx::data::GetScaling(
        tdx::data::ClassifySecurity(q.code), tdx::data::DataSource::NetQuotes);
    q.price     = static_cast<double>(base) * s.price;
    q.pre_close = static_cast<double>(base + pre_close.value) * s.pre_close;
    q.open      = static_cast<double>(base + open.value) * s.ohlc;
    q.high      = static_cast<double>(base + high.value) * s.ohlc;
    q.low       = static_cast<double>(base + low.value) * s.ohlc;
    q.volume    = static_cast<double>(vol.value) * s.volume;
    q.amount    = static_cast<double>(amount) * s.amount;

    // 五档 ×5：每档 bid/ask 相对 price 增量（quotes_detail.py:57-58）
    for (int j = 0; j < 5; ++j) {
      auto bid = get_price(data, len, pos); pos = bid.new_pos;
      auto ask = get_price(data, len, pos); pos = ask.new_pos;
      auto bid_vol = get_price(data, len, pos); pos = bid_vol.new_pos;
      auto ask_vol = get_price(data, len, pos); pos = ask_vol.new_pos;
      q.bid[j]     = static_cast<double>(base + bid.value) * s.bid_ask;
      q.ask[j]     = static_cast<double>(base + ask.value) * s.bid_ask;
      q.bid_vol[j] = static_cast<double>(bid_vol.value) * s.volume;
      q.ask_vol[j] = static_cast<double>(ask_vol.value) * s.volume;
    }

    if (pos + 10 > len) break;
    pos += 10;  // 尾部 <h4shH>：unknown/rise_speed/active2（暂不解析）

    result.push_back(std::move(q));
  }
  return result;
}

// ---- 除权除息 0x0f（对齐 opentdx parser/quotation/company_info.py:XDXR）----
std::vector<uint8_t> serialize_xdxr(Market market, std::string_view code) {
  // 请求体 <HB6s>：type(H=1) + market(B) + code(6s GBK)
  std::vector<uint8_t> body;
  body.reserve(9);
  push_u16(body, 1);  // type
  body.push_back(static_cast<uint8_t>(market));
  push_code(body, code, 6);
  return body;
}

std::vector<Xdxr> deserialize_xdxr(const uint8_t* data, std::size_t len) {
  std::vector<Xdxr> result;
  if (len < 11) return result;

  // 头部 <HB6sH>：market(H) marketOR(B) code(6s) count(H) = 11 字节
  uint16_t count = util::rd_u16(data + 9);
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t pos = 11 + static_cast<std::size_t>(i) * 29;
    if (pos + 29 > len) break;

    // <B6sBIB>：market(B) code(6s) unknown(B) date(I) category(B) = 13 字节
    uint32_t date_raw = util::rd_u32(data + pos + 8);
    uint8_t category = data[pos + 12];

    Xdxr x;
    // date: YYYYMMDD → "YYYY-MM-DD"
    char dbuf[16];
    int y = static_cast<int>(date_raw / 10000);
    int m = static_cast<int>((date_raw % 10000) / 100);
    int d = static_cast<int>(date_raw % 100);
    std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", y, m, d);
    x.date = dbuf;
    x.category = category;

    // 后 16 字节：按 category 解析
    const uint8_t* ld = data + pos + 13;
    if (category == 1) {
      x.fenhong = static_cast<double>(util::rd_f32(ld));
      x.peigujia = static_cast<double>(util::rd_f32(ld + 4));
      x.songzhuangu = static_cast<double>(util::rd_f32(ld + 8));
      x.peigu = static_cast<double>(util::rd_f32(ld + 12));
      x.name = "除权除息";
    } else {
      // category != 1 的字段不解析，仅保留 date/category/name
      switch (category) {
        case 2: x.name = "送配股上市"; break;
        case 3: x.name = "非流通股上市"; break;
        case 4: x.name = "未知股本变动"; break;
        case 5: x.name = "股本变化"; break;
        case 6: x.name = "增发新股"; break;
        case 7: x.name = "股份回购"; break;
        case 8: x.name = "增发新股上市"; break;
        case 9: x.name = "转配股上市"; break;
        case 10: x.name = "可转债上市"; break;
        case 11: x.name = "扩缩股"; break;
        case 12: x.name = "非流通股缩股"; break;
        case 13: x.name = "送认购权证"; break;
        case 14: x.name = "送认沽权证"; break;
        default: x.name = "未知"; break;
      }
    }
    result.push_back(std::move(x));
  }
  return result;
}

}  // namespace tdx::proto
