// SP/MAC parser（板块/K线/异动/资金流/竞价）。逐字移植 opentdx/parser/mac_quotation/*。
// SP 帧头与标准一致（head=0x0c）；code 长度多为 22s；0x1218 靠内嵌 ASCII 串区分业务。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/proto/bitmap.hpp"
#include "tdx/proto/sp_codec.hpp"  // exchange_board_code（board_symbol→board_code）
#include "tdx/types.hpp"

namespace tdx::proto {

// SP msg_id 常量
inline constexpr uint16_t kMsgSpBoardList = 0x1231;
inline constexpr uint16_t kMsgSpBoardMembers = 0x122C;
inline constexpr uint16_t kMsgSpSymbolQuotes = 0x122B;
inline constexpr uint16_t kMsgSpSymbolBar = 0x122E;
inline constexpr uint16_t kMsgSpUnusual = 0x1237;
inline constexpr uint16_t kMsgSpCapitalFlow = 0x1218;
inline constexpr uint16_t kMsgSpAuction = 0x123D;

// ---- 板块列表 0x1231（请求 <HHBBHH8x>；响应 <HH>头 + 160B/块 board+symbol）----
struct SpBoard {
  uint16_t market = 0;
  std::string code;
  std::string name;
  double price = 0.0;
  double rise_speed = 0.0;
  double pre_close = 0.0;
};
std::vector<uint8_t> serialize_sp_board_list(BoardType board_type, uint16_t start, uint16_t page_size);
std::vector<SpBoard> deserialize_sp_board_list(const uint8_t* data, std::size_t len);

// ---- 板块成员报价 0x122C（位图驱动，复用 deserialize_symbol_quotes）----
// 请求 = <I9xHIBBBB>(board_code, sort_type, start, page_size, 0, sort_order, 0) + 20B 位图
std::vector<uint8_t> serialize_sp_board_members(int board_code, SortType sort_type,
                                                uint16_t start, uint16_t page_size,
                                                SortOrder sort_order,
                                                const std::vector<FieldBit>& fields);

// ---- 单标的报价 0x122B（位图驱动，响应复用 deserialize_symbol_quotes 解析）----
// 请求 = 20B 位图 + <H>count + N×<H22s>(market, code_gbk)。market 为 MARKET/EX_MARKET 数值。
std::vector<uint8_t> serialize_sp_symbol_quotes(
    const std::vector<std::pair<uint16_t, std::string>>& codes,
    const std::vector<FieldBit>& fields);

// ---- SP K线 0x122E（请求 22s code；响应 33B 头 + 36B/根 <II7f>）----
std::vector<uint8_t> serialize_sp_symbol_bar(uint16_t market, std::string_view code,
                                             Period period, uint16_t start, uint16_t count);
std::vector<KLine> deserialize_sp_symbol_bar(const uint8_t* data, std::size_t len, Period period);

// ---- 异动 0x1237（请求 <HH2xH2xH5H>；响应 <H>count + 32B/条）----
struct SpUnusual {
  uint16_t market = 0;
  std::string code;
  uint8_t type = 0;
  std::string desc;  // 简化描述（完整 unpack_by_type 推迟）
};
std::vector<uint8_t> serialize_sp_unusual(uint16_t market, uint16_t start, uint16_t count);
std::vector<SpUnusual> deserialize_sp_unusual(const uint8_t* data, std::size_t len);

// ---- 资金流 0x1218 + "Stock_ZJLX"（请求 <H8s16x21s>；响应 27B 头 + GBK JSON）----
struct SpCapitalFlow {
  double main_net = 0.0;       // 今日主力净额
  double small_net = 0.0;      // 今日散户净额
  double five_day_main = 0.0;  // 5日主力
};
std::vector<uint8_t> serialize_sp_capital_flow(uint16_t market, std::string_view code);
std::vector<SpCapitalFlow> deserialize_sp_capital_flow(const uint8_t* data, std::size_t len);

// ---- 集合竞价 0x123D（请求 <H22sII10x>；响应 36B 头 + 16B/条 <IfIi>）----
struct SpAuction {
  int64_t datetime = 0;  // 当日秒数
  double price = 0.0;
  int32_t matched = 0;
  int32_t unmatched = 0;
};
std::vector<uint8_t> serialize_sp_auction(uint16_t market, std::string_view code,
                                          uint32_t start, uint32_t count);
std::vector<SpAuction> deserialize_sp_auction(const uint8_t* data, std::size_t len);

}  // namespace tdx::proto
