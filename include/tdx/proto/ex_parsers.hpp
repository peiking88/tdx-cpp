// 扩展行情 parser（端口 7727，head=1）。逐字移植 opentdx/parser/ex_quotation/*。
// 关键：所有请求帧 head=0x01（非 0x0c），customize=0；Login 是 80 字节固定 hex；
//       扩展行情不发心跳（ExtQuotes 不启动 Heartbeat）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/types.hpp"

#include <utility>

namespace tdx::proto {

// 扩展行情 msg_id 常量
inline constexpr uint16_t kMsgExLogin = 0x2454;
inline constexpr uint16_t kMsgExInfo = 0x2455;
inline constexpr uint16_t kMsgExCount = 0x23f0;
inline constexpr uint16_t kMsgExCategoryList = 0x23f4;
inline constexpr uint16_t kMsgExList = 0x23f5;
inline constexpr uint16_t kMsgExKline = 0x23ff;
inline constexpr uint16_t kMsgExKline2 = 0x2489;
inline constexpr uint16_t kMsgExQuotesSingle = 0x23fa;
inline constexpr uint16_t kMsgExQuotes = 0x248a;
inline constexpr uint16_t kMsgExQuotesList = 0x2484;
inline constexpr uint16_t kMsgExHistoryTxn = 0x2412;
inline constexpr uint16_t kMsgExTickChart = 0x248b;
inline constexpr uint16_t kMsgExHistoryTickChart = 0x248c;

// 扩展行情所有请求帧的 head 字节（对齐 baseParser register_parser 第 2 参数=1）
inline constexpr uint8_t kExHead = 0x01;

// ---- 登录 0x2454：80 字节固定 hex body ----
std::vector<uint8_t> serialize_ex_login();

struct ExLoginResult {
  std::string date_time;  // YYYY-MM-DD HH:MM:SS
  std::string server_name;
  std::string desc;
  std::string ip;
};
// 解析登录响应 <B52sHBBBBBB21sfBHHH151sBBB52s>（299 字节）
ExLoginResult deserialize_ex_login(const uint8_t* data, std::size_t len);

// ---- K 线 0x23ff（请求 <B9sHHIH>；响应 20B 回显 + 32B/根 <IfffffIf>，OHLC float32）----
// 注意：扩展 K 线 OHLC 顺序是 open/high/low/close（标准顺序），与标准行情 open/close/high/low 不同。
std::vector<uint8_t> serialize_ex_kline(ExMarket market, std::string_view code, Period period,
                                        uint16_t times, uint32_t start, uint16_t count);
std::vector<KLine> deserialize_ex_kline(const uint8_t* data, std::size_t len, Period period);

// ---- K 线2 0x2489（请求 <B23sHHII16x>；响应 42B 回显 + 32B/根 <IfffffII>）----
std::vector<uint8_t> serialize_ex_kline2(ExMarket market, std::string_view code, Period period,
                                         uint16_t times, uint32_t start, uint32_t count);
std::vector<KLine> deserialize_ex_kline2(const uint8_t* data, std::size_t len, Period period);

// ---- 数量 0x23f0（空 body；响应 <11s5I>，返回 count）----
std::vector<uint8_t> serialize_ex_count();
uint32_t deserialize_ex_count(const uint8_t* data, std::size_t len);

// ---- 类别列表 0x23f4（空 body；响应 <H>count + 64B/条 <B32sB30s>）----
struct ExCategory {
  uint8_t goods_type = 0;
  std::string name;  // UTF8
  uint8_t code = 0;
  std::string abbr;  // UTF8
};
std::vector<ExCategory> deserialize_ex_category_list(const uint8_t* data, std::size_t len);

// ---- 列表 0x23f5（请求 <IH>；响应 <IH>头 + 64B/条）----
struct ExListItem {
  uint8_t market = 0;
  uint8_t category = 0;
  std::string code;
  std::string name;
};
std::vector<uint8_t> serialize_ex_list(uint32_t start, uint16_t count);
std::vector<ExListItem> deserialize_ex_list(const uint8_t* data, std::size_t len);

// ---- 报价 0x248a（请求 <B7xH> + N×<B23s>；响应 <IIH>头 + 314B/条 unpack_futures）----
std::vector<uint8_t> serialize_ex_quotes(const std::vector<std::pair<ExMarket, std::string>>& codes);
std::vector<ExQuote> deserialize_ex_quotes(const uint8_t* data, std::size_t len);

// ---- 历史成交 0x2412（请求 <IB43sH>；响应 58B 头 + 16B/笔 <HIIIH>）----
std::vector<uint8_t> serialize_ex_history_txn(ExMarket market, std::string_view code, int ymd);
std::vector<Transaction> deserialize_ex_history_txn(const uint8_t* data, std::size_t len);

}  // namespace tdx::proto
