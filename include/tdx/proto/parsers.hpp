// 通达信行情 parser。逐字移植 opentdx/parser/quotation/* 的请求构造与响应解析。
// 采用函数式 serialize/deserialize（简单优先，不做运行时 Registry 抽象）：
//   请求：serialize_xxx(...) → 请求体字节（再由 frame::pack_request 包头）
//   响应：deserialize_xxx(body, len) → 结构体 vector（body 已由 connection 解压）
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/types.hpp"

namespace tdx::proto {

// msg_id 常量（对齐 opentdx register_parser）
inline constexpr uint16_t kMsgLogin = 0x0d;
inline constexpr uint16_t kMsgHeartbeat = 0x04;
inline constexpr uint16_t kMsgXdxr = 0x0f;
inline constexpr uint16_t kMsgKline = 0x523;
inline constexpr uint16_t kMsgTick = 0x537;
inline constexpr uint16_t kMsgTransaction = 0xfc5;
inline constexpr uint16_t kMsgQuotesDetail = 0x53e;
inline constexpr uint16_t kMsgList = 0x44d;
inline constexpr uint16_t kMsgCount = 0x44e;
inline constexpr uint16_t kMsgFinance = 0x10;
inline constexpr uint16_t kMsgF10Category = 0x2cf;
inline constexpr uint16_t kMsgF10Content = 0x2d0;
inline constexpr uint16_t kMsgHistoryOrders = 0xfb4;
inline constexpr uint16_t kMsgHistoryTransaction = 0xfb5;
inline constexpr uint16_t kMsgVolumeProfile = 0x51a;
inline constexpr uint16_t kMsgIndexInfo = 0x51d;
inline constexpr uint16_t kMsgUnusual = 0x563;

// ---- K 线 0x523（请求 <H6sHHHHH8s；响应 count + 变长 OHLC[open/close/high/low] + vol/amount + 可选 upCount）----
std::vector<uint8_t> serialize_kline(Market market, std::string_view code, Period period,
                                     uint16_t times, uint16_t start, uint16_t count,
                                     Adjust adjust);
std::vector<KLine> deserialize_kline(const uint8_t* data, std::size_t len, Period period);

// ---- 分时 0x537（请求 <H6sHH；响应 num + 变长 price/avg/vol 双增量）----
std::vector<uint8_t> serialize_tick(Market market, std::string_view code,
                                    uint16_t start, uint16_t count);
std::vector<Tick> deserialize_tick(const uint8_t* data, std::size_t len);

// ---- 逐笔 0xfc5（请求 <H6sHH；响应 count + minutes + 变长 price/vol/trans/buy_sell/unknown，price 增量）----
std::vector<uint8_t> serialize_transaction(Market market, std::string_view code,
                                           uint16_t start, uint16_t count);
std::vector<Transaction> deserialize_transaction(const uint8_t* data, std::size_t len);

// ---- 列表 0x44d（请求 <H3I；响应 count + 37B/条 <6sH16sfBfHH）----
std::vector<uint8_t> serialize_list(Market market, uint16_t start, uint16_t count);
std::vector<Stock> deserialize_list(const uint8_t* data, std::size_t len);

// ---- 数量 0x44e（请求 <HI；响应 uint16 总数）----
std::vector<uint8_t> serialize_count(Market market);
uint16_t deserialize_count(const uint8_t* data, std::size_t len);

// ---- 五档报价 0x53e（请求 <H6sH + N×<B6s；响应 price 基准 + OHLC/五档相对增量）----
struct QuoteReq {
  Market market;
  std::string code;
};
std::vector<uint8_t> serialize_quotes_detail(const std::vector<QuoteReq>& stocks);
std::vector<Quote> deserialize_quotes_detail(const uint8_t* data, std::size_t len);

// ---- 登录 0x0d（请求体 <B=1）----
std::vector<uint8_t> serialize_login();

// ---- 心跳 0x04（请求体空）----
std::vector<uint8_t> serialize_heartbeat();

// ---- 除权除息 0x0f（请求 <HB6s；响应 count + N×29B）----
std::vector<uint8_t> serialize_xdxr(Market market, std::string_view code);
std::vector<Xdxr> deserialize_xdxr(const uint8_t* data, std::size_t len);

// ---- 财务 0x10（请求 <HB6s；响应 29 f32 字段）----
std::vector<uint8_t> serialize_finance(Market market, std::string_view code);
Finance deserialize_finance(const uint8_t* data, std::size_t len);

// ---- F10 分类 0x2cf（请求 <H6sI；响应 count + 152B/条）----
std::vector<uint8_t> serialize_f10_category(Market market, std::string_view code);
std::vector<F10Category> deserialize_f10_category(const uint8_t* data, std::size_t len);

// ---- F10 内容 0x2d0（请求 <H6sH80sIII；响应 12B 头 + GBK 文本）----
std::vector<uint8_t> serialize_f10_content(Market market, std::string_view code,
                                           std::string_view filename, uint32_t start, uint32_t length);
F10Content deserialize_f10_content(const uint8_t* data, std::size_t len);

// ---- 历史委托 0xfb4（请求 <IB6s；响应 pre_close + count×变长 price/unknown/vol）----
std::vector<uint8_t> serialize_history_orders(Market market, std::string_view code, uint32_t date_yyyymmdd);
std::vector<HistoryOrder> deserialize_history_orders(const uint8_t* data, std::size_t len);

// ---- 历史逐笔 0xfb5（请求 <IH6sHH；响应 pre_close + count×<H+变长 price/vol/buy_sell/unknown>）----
std::vector<uint8_t> serialize_history_transaction(Market market, std::string_view code,
                                                    uint32_t date_yyyymmdd, uint16_t start, uint16_t count);
std::vector<HistoryTransaction> deserialize_history_transaction(const uint8_t* data, std::size_t len);

// ---- 成交量分布 0x51a（请求 <H6s；响应 含 vol_profile 数组）----
std::vector<uint8_t> serialize_volume_profile(Market market, std::string_view code);
VolProfile deserialize_volume_profile(const uint8_t* data, std::size_t len);

// ---- 指数信息 0x51d（请求 <H6sI；响应 含涨跌家数）----
std::vector<uint8_t> serialize_index_info(Market market, std::string_view code);
IndexInfo deserialize_index_info(const uint8_t* data, std::size_t len);

// ---- 主力异动 0x563（请求 <HII；响应 count + 34B/条）----
std::vector<uint8_t> serialize_unusual(Market market, uint16_t start, uint16_t count);
std::vector<UnusualItem> deserialize_unusual(const uint8_t* data, std::size_t len);

}  // namespace tdx::proto
