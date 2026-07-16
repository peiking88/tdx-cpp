// SP parser 实现。逐字移植 opentdx/parser/mac_quotation/*。
#include "tdx/proto/sp_parsers.hpp"

#include <cstring>

#include <nlohmann/json.hpp>

#include "tdx/proto/bitmap.hpp"
#include "tdx/proto/sp_codec.hpp"  // combine_to_datetime
#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"

namespace tdx::proto {
using tdx::util::push_code;
using tdx::util::push_u8;
using tdx::util::push_u16;
using tdx::util::push_u32;

// ============================ 板块列表 0x1231 ============================

std::vector<uint8_t> serialize_sp_board_list(BoardType board_type, uint16_t start, uint16_t page_size) {
  // <HHBBHH8x>: page_size(H), board_type(H), sort_column(B)=0, sort_order(B)=1, start(H), 1(H), 8x
  std::vector<uint8_t> body;
  push_u16(body, page_size);
  push_u16(body, static_cast<uint16_t>(board_type));
  push_u8(body, 0);   // sort_column
  push_u8(body, 1);   // sort_order
  push_u16(body, start);
  push_u16(body, 1);
  for (int i = 0; i < 8; ++i) body.push_back(0);
  return body;
}

std::vector<SpBoard> deserialize_sp_board_list(const uint8_t* data, std::size_t len) {
  // board_list.py:22-46：<HH>头(count_all, total)，count=count_all/2，每块 160B（board 80B + symbol 80B）
  std::vector<SpBoard> result;
  if (len < 4) return result;
  uint16_t count_all = util::rd_u16(data);
  int count = count_all / 2;
  for (int i = 0; i < count; ++i) {
    std::size_t off = 4 + static_cast<std::size_t>(i) * 160;
    if (off + 160 > len) break;
    const uint8_t* p = data + off;
    // board: market(H)@0 code(6s)@2 16x@8 name(44s)@24 price(f)@68 rise_speed(f)@72 pre_close(f)@76
    SpBoard b;
    b.market = util::rd_u16(p);
    b.code = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 2), 6));
    b.name = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 24), 44));
    b.price = util::rd_f32(p + 68);
    b.rise_speed = util::rd_f32(p + 72);
    b.pre_close = util::rd_f32(p + 76);
    result.push_back(std::move(b));
  }
  return result;
}

// ============================ 板块成员报价 0x122C ============================

std::vector<uint8_t> serialize_sp_board_members(int board_code, SortType sort_type,
                                                uint16_t start, uint16_t page_size,
                                                SortOrder sort_order,
                                                const std::vector<FieldBit>& fields) {
  // <I9xHIBBBB>(board_code, sort_type, start, page_size, 0, sort_order, 0) + 20B 位图
  std::vector<uint8_t> body;
  push_u32(body, static_cast<uint32_t>(board_code));
  for (int i = 0; i < 9; ++i) body.push_back(0);
  push_u16(body, static_cast<uint16_t>(sort_type));
  push_u32(body, start);
  push_u8(body, static_cast<uint8_t>(page_size));
  push_u8(body, 0);
  push_u8(body, static_cast<uint8_t>(sort_order));
  push_u8(body, 0);
  auto bitmap = build_bitmap(fields);
  body.insert(body.end(), bitmap.begin(), bitmap.end());
  return body;
}

// ============================ 单标的报价 0x122B ============================

std::vector<uint8_t> serialize_sp_symbol_quotes(
    const std::vector<std::pair<uint16_t, std::string>>& codes,
    const std::vector<FieldBit>& fields) {
  // 20B 位图 + <H>count + N×<H22s>(market, code_gbk)。响应用 deserialize_symbol_quotes。
  std::vector<uint8_t> body;
  auto bitmap = build_bitmap(fields);
  body.insert(body.end(), bitmap.begin(), bitmap.end());
  push_u16(body, static_cast<uint16_t>(codes.size()));
  for (const auto& [market, code] : codes) {
    push_u16(body, market);
    push_code(body, code, 22);
  }
  return body;
}

// ============================ SP K线 0x122E ============================

std::vector<uint8_t> serialize_sp_symbol_bar(uint16_t market, std::string_view code,
                                             Period period, uint16_t start, uint16_t count) {
  // <H22sHHIHHbbb bH4s>（简化核心：market, code22, period, times, start, count, fq, 尾部）
  std::vector<uint8_t> body;
  push_u16(body, market);
  push_code(body, code, 22);
  push_u16(body, static_cast<uint16_t>(period));
  push_u16(body, 1);  // times
  push_u32(body, start);
  push_u16(body, count);
  push_u16(body, 0);  // fq
  push_u8(body, 1); push_u8(body, 1); push_u8(body, 0);  // bbb
  push_u8(body, 1);  // b
  push_u16(body, 0);
  for (int i = 0; i < 4; ++i) body.push_back(0);  // 4s
  return body;
}

std::vector<KLine> deserialize_sp_symbol_bar(const uint8_t* data, std::size_t len, Period period) {
  // symbol_bar.py:16-44：33B 头 <H12s10xBHHI> + 36B/根 <II7f>
  std::vector<KLine> bars;
  if (len < 33) return bars;
  uint16_t count = util::rd_u16(data + 27);  // market H@0 symbol 12s@2 10x@14 period B@24 _ H@25 count H@27 start I@29
  (void)period;
  for (uint16_t i = 0; i < count; ++i) {
    std::size_t off = 33 + static_cast<std::size_t>(i) * 36;
    if (off + 36 > len) break;
    const uint8_t* p = data + off;
    uint32_t ymd = util::rd_u32(p);
    uint32_t time_num = util::rd_u32(p + 4);
    KLine bar;
    bar.datetime = combine_to_datetime(ymd, time_num);
    bar.open = util::rd_f32(p + 8);
    bar.high = util::rd_f32(p + 12);
    bar.low = util::rd_f32(p + 16);
    bar.close = util::rd_f32(p + 20);
    bar.amount = util::rd_f32(p + 24);
    bar.volume = util::rd_f32(p + 28);
    bars.push_back(bar);
  }
  return bars;
}

// ============================ 异动 0x1237 ============================

std::vector<uint8_t> serialize_sp_unusual(uint16_t market, uint16_t start, uint16_t count) {
  // <HH2xH2xH5H>: market, start, 2x, count, 2x, 1, 200, 30, 40, 50, 200
  std::vector<uint8_t> body;
  push_u16(body, market);
  push_u16(body, start);
  push_u16(body, 0);  // 2x
  push_u16(body, count);
  push_u16(body, 0);  // 2x
  push_u16(body, 1);
  push_u16(body, 200);
  push_u16(body, 30);
  push_u16(body, 40);
  push_u16(body, 50);
  push_u16(body, 200);
  return body;
}

std::vector<SpUnusual> deserialize_sp_unusual(const uint8_t* data, std::size_t len) {
  // unusual.py:13-37：<H>count + 32B/条（15B 头段 <H6sBBBHH> + 13B value + 3B 时间 + 尾部 GBK 文本）
  std::vector<SpUnusual> result;
  if (len < 2) return result;
  uint16_t count = util::rd_u16(data);
  std::size_t pos = 2;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 32 > len) break;
    const uint8_t* p = data + pos;
    // <H6sBBBHH>: market H@0, code 6s@2, _ B@8, unusual_type B@9, _ B@10, index H@11, z H@13
    SpUnusual u;
    u.market = util::rd_u16(p);
    u.code = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 2), 6));
    u.type = util::rd_u8(p + 9);
    u.desc = "type=0x" + ([](uint8_t v) { char b[8]; std::snprintf(b, sizeof(b), "%02x", v); return std::string(b); })(u.type);
    result.push_back(std::move(u));
    pos += 32;
  }
  return result;
}

// ============================ 资金流 0x1218 + Stock_ZJLX ============================

std::vector<uint8_t> serialize_sp_capital_flow(uint16_t market, std::string_view code) {
  // <H8s16x21s>: market, code(8s), 16x, "Stock_ZJLX"(21s ASCII)
  std::vector<uint8_t> body;
  push_u16(body, market);
  push_code(body, code, 8);
  for (int i = 0; i < 16; ++i) body.push_back(0);
  push_code(body, "Stock_ZJLX", 21);
  return body;
}

std::vector<SpCapitalFlow> deserialize_sp_capital_flow(const uint8_t* data, std::size_t len) {
  // symbol_capital_flow.py:12-33：27B 头 + GBK JSON 数组（today[4] + five_days[6]）。
  // 通达信服务器 JSON 数值为字符串格式（"123.45"），需兼容解析。
  std::vector<SpCapitalFlow> result;
  if (len < 27) return result;
  std::string json_str = util::gbk_to_utf8(reinterpret_cast<const char*>(data + 27), len - 27);
  // 截断尾随 null padding
  size_t nul = json_str.find('\0');
  if (nul != std::string::npos) json_str.resize(nul);
  auto GetNum = [](const nlohmann::json& v) -> double {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) return std::stod(v.get<std::string>());
    return 0.0;
  };
  SpCapitalFlow cf;
  try {
    auto j = nlohmann::json::parse(json_str);
    if (j.size() >= 1 && j[0].is_array() && j[0].size() >= 4) {
      cf.main_net  = GetNum(j[0][0]) - GetNum(j[0][1]);
      cf.small_net = GetNum(j[0][2]) - GetNum(j[0][3]);
    }
    if (j.size() >= 2 && j[1].is_array() && j[1].size() >= 2) {
      cf.five_day_main = GetNum(j[1][0]) - GetNum(j[1][1]);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[WARN] capital_flow json parse: %s\n", e.what());
    return result;
  }
  result.push_back(cf);
  return result;
}

// ============================ 集合竞价 0x123D ============================

std::vector<uint8_t> serialize_sp_auction(uint16_t market, std::string_view code,
                                          uint32_t start, uint32_t count) {
  // <H22sII10x>: market, code(22s), start, count, 10x
  std::vector<uint8_t> body;
  push_u16(body, market);
  push_code(body, code, 22);
  push_u32(body, start);
  push_u32(body, count);
  for (int i = 0; i < 10; ++i) body.push_back(0);
  return body;
}

std::vector<SpAuction> deserialize_sp_auction(const uint8_t* data, std::size_t len) {
  // symbol_auction.py:11-25：36B 头 + 16B/条 <IfIi>
  std::vector<SpAuction> result;
  if (len < 36) return result;
  // count 在头中（offset 实测，agent 说偏移 28）
  uint32_t count = util::rd_u32(data + 28);
  std::size_t pos = 36;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 16 > len) break;
    const uint8_t* p = data + pos;
    SpAuction a;
    a.datetime = util::rd_u32(p);       // 当日秒数
    a.price = util::rd_f32(p + 4);
    a.matched = static_cast<int32_t>(util::rd_u32(p + 8));
    a.unmatched = static_cast<int32_t>(util::rd_u32(p + 12));
    result.push_back(a);
    pos += 16;
  }
  return result;
}

}  // namespace tdx::proto
