// SP 位图动态字段机制（0x122B SymbolQuotes / 0x122C BoardMembersQuotes）。
// 对齐 opentdx utils/bitmap.py：20 字节位图（160 位），每激活位对应一个字段（4 字节）。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tdx::proto {

// 位图字段位（对齐 bitmap.py FieldBit，核心子集；未列位按 unknown_field_{bit} 处理）
enum class FieldBit : uint8_t {
  PreClose = 0x00, Open = 0x01, High = 0x02, Low = 0x03, Close = 0x04,
  Vol = 0x05, VolRatio = 0x06, Amount = 0x07,
  InsideVolume = 0x08, OutsideVolume = 0x09,
  TotalShares = 0x0a, FloatShares = 0x0b, Eps = 0x0c, NetAssets = 0x0d,
  ChangePct = 0x0e,
  TotalMarketCapAb = 0x0f, PeDynamic = 0x10,
  Bid = 0x11, Ask = 0x12,
  ServerUpdateDate = 0x13, ServerUpdateTime = 0x14,
  DividendYield = 0x17, BidVolume = 0x18, AskVolume = 0x19,
  LastVolume = 0x1a, Turnover = 0x1b,
  DecimalPoint = 0x1f, BuyPriceLimit = 0x20, SellPriceLimit = 0x21,
  SpeedPct = 0x25, AvgPrice = 0x26,
  PeTtm = 0x30, PeStatic = 0x31,
  Change20dPct = 0x3b, YtdPct = 0x3c, MtdPct = 0x40,
  Change1yPct = 0x41, PrevChangePct = 0x42,
  Change3dPct = 0x43, Change60dPct = 0x44, Change5dPct = 0x45, Change10dPct = 0x46,
  Activity = 0x59, LimitUpCount = 0x5d, LimitDownCount = 0x5e,
  UpCount = 0x88, DownCount = 0x8b,
};

// 字段格式（4 字节统一，但解读不同）
enum class FieldFmt : uint8_t { Float, UInt, Int };

// 构造 20 字节位图（对齐 bitmap.py:326-333 build_bitmap）
std::array<uint8_t, 20> build_bitmap(const std::vector<FieldBit>& fields);

// 预设字段集（对齐 bitmap.py:265-296 PresetField）
std::vector<FieldBit> PresetBasic();    // PRE_CLOSE|OPEN|HIGH|LOW|CLOSE|VOL
std::vector<FieldBit> PresetQuote();    // BID|ASK|BID_VOLUME|ASK_VOLUME|LAST_VOLUME
std::vector<FieldBit> PresetCommon();   // 默认组合（简化版）

// 查询位 → (字段名, 格式)。未知位返回 ("unknown_field_{bit}", Float)。
std::pair<std::string, FieldFmt> FieldDefAt(int bit);

// 位图驱动的报价行（0x122B/0x122C 响应共用）
struct SymbolQuoteRow {
  uint16_t market = 0;
  std::string code;   // UTF8
  std::string name;   // UTF8
  // 按 bit 升序的 (字段名, 值)，未激活位不出现
  std::vector<std::pair<std::string, double>> fields;
};

// 解析位图响应：前 20B 位图 + <IH>(total,row_count) + 每行 68+4*popcount。
std::vector<SymbolQuoteRow> deserialize_symbol_quotes(const uint8_t* data, std::size_t len);

}  // namespace tdx::proto
