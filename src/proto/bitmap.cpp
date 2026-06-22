// 位图机制实现。对齐 opentdx utils/bitmap.py + symbol_quotes.py。
#include "tdx/proto/bitmap.hpp"

#include <cstring>

#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"

namespace tdx::proto {

std::array<uint8_t, 20> build_bitmap(const std::vector<FieldBit>& fields) {
  // bitmap.py:326-333：20 字节，bitmap[bit/8] |= 1<<(bit%8)
  std::array<uint8_t, 20> bitmap{};
  for (FieldBit f : fields) {
    int bit = static_cast<int>(f);
    if (bit >= 0 && bit < 160) {
      bitmap[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
  }
  return bitmap;
}

std::vector<FieldBit> PresetBasic() {
  return {FieldBit::PreClose, FieldBit::Open, FieldBit::High, FieldBit::Low,
          FieldBit::Close, FieldBit::Vol};
}

std::vector<FieldBit> PresetQuote() {
  return {FieldBit::Bid, FieldBit::Ask, FieldBit::BidVolume, FieldBit::AskVolume,
          FieldBit::LastVolume};
}

std::vector<FieldBit> PresetCommon() {
  // 简化版常用组合（对齐 bitmap.py COMMON 的核心子集）
  return {FieldBit::PreClose, FieldBit::Open,  FieldBit::High,    FieldBit::Low,
          FieldBit::Close,    FieldBit::Vol,   FieldBit::Amount,  FieldBit::Bid,
          FieldBit::Ask,      FieldBit::BidVolume, FieldBit::AskVolume, FieldBit::Turnover,
          FieldBit::PeTtm,    FieldBit::ChangePct};
}

std::pair<std::string, FieldFmt> FieldDefAt(int bit) {
  // 对齐 bitmap.py:5-105 FIELD_BITMAP_MAP（核心位；其余返回 unknown）
  switch (bit) {
    case 0x00: return {"pre_close", FieldFmt::Float};
    case 0x01: return {"open", FieldFmt::Float};
    case 0x02: return {"high", FieldFmt::Float};
    case 0x03: return {"low", FieldFmt::Float};
    case 0x04: return {"close", FieldFmt::Float};
    case 0x05: return {"vol", FieldFmt::UInt};
    case 0x06: return {"vol_ratio", FieldFmt::Float};
    case 0x07: return {"amount", FieldFmt::Float};
    case 0x0a: return {"total_shares", FieldFmt::Float};
    case 0x0b: return {"float_shares", FieldFmt::Float};
    case 0x0c: return {"eps", FieldFmt::Float};
    case 0x0d: return {"net_assets", FieldFmt::Float};
    case 0x0e: return {"change_pct", FieldFmt::Float};
    case 0x0f: return {"total_market_cap_ab", FieldFmt::Float};
    case 0x10: return {"pe_dynamic", FieldFmt::Float};
    case 0x11: return {"bid", FieldFmt::Float};
    case 0x12: return {"ask", FieldFmt::Float};
    case 0x18: return {"bid_volume", FieldFmt::UInt};
    case 0x19: return {"ask_volume", FieldFmt::UInt};
    case 0x1a: return {"last_volume", FieldFmt::UInt};
    case 0x1b: return {"turnover", FieldFmt::Float};
    case 0x20: return {"buy_price_limit", FieldFmt::Float};
    case 0x21: return {"sell_price_limit", FieldFmt::Float};
    case 0x25: return {"speed_pct", FieldFmt::Float};
    case 0x26: return {"avg_price", FieldFmt::Float};
    case 0x30: return {"pe_ttm", FieldFmt::Float};
    case 0x31: return {"pe_static", FieldFmt::Float};
    case 0x3b: return {"change_20d_pct", FieldFmt::Float};
    case 0x3c: return {"ytd_pct", FieldFmt::Float};
    case 0x42: return {"prev_change_pct", FieldFmt::Float};
    case 0x43: return {"change_3d_pct", FieldFmt::Float};
    case 0x44: return {"change_60d_pct", FieldFmt::Float};
    case 0x45: return {"change_5d_pct", FieldFmt::Float};
    case 0x46: return {"change_10d_pct", FieldFmt::Float};
    case 0x59: return {"activity", FieldFmt::UInt};
    case 0x5d: return {"limit_up_count", FieldFmt::UInt};
    case 0x5e: return {"limit_down_count", FieldFmt::UInt};
    case 0x88: return {"up_count", FieldFmt::UInt};
    case 0x8b: return {"down_count", FieldFmt::UInt};
    default: return {"unknown_field_" + std::to_string(bit), FieldFmt::Float};
  }
}

std::vector<SymbolQuoteRow> deserialize_symbol_quotes(const uint8_t* data, std::size_t len) {
  // symbol_quotes.py:19-55：前 20B 位图 + <IH>(total@20, row_count@24)
  std::vector<SymbolQuoteRow> result;
  if (len < 26) return result;

  // 收集激活位（bit 升序）+ popcount
  std::vector<int> active_bits;
  for (int byte = 0; byte < 20; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      if (data[byte] & (1u << bit)) active_bits.push_back(byte * 8 + bit);
    }
  }
  uint16_t row_count = util::rd_u16(data + 24);
  std::size_t row_len = 68 + 4 * active_bits.size();  // 每行 <H22s44s>=68 + 4*popcount
  std::size_t pos = 26;

  for (uint16_t i = 0; i < row_count; ++i) {
    if (pos + row_len > len) break;
    const uint8_t* p = data + pos;
    SymbolQuoteRow row;
    row.market = util::rd_u16(p);
    row.code = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 2), 22));
    row.name = util::trim_null(util::gbk_to_utf8(reinterpret_cast<const char*>(p + 24), 44));
    std::size_t fpos = 68;
    for (int bit : active_bits) {
      auto [name, fmt] = FieldDefAt(bit);
      double val;
      if (fmt == FieldFmt::Float) {
        val = util::rd_f32(p + fpos);
      } else {
        val = static_cast<double>(util::rd_u32(p + fpos));
      }
      row.fields.emplace_back(std::move(name), val);
      fpos += 4;
    }
    result.push_back(std::move(row));
    pos += row_len;
  }
  return result;
}

}  // namespace tdx::proto
