// tdx 协议常量与枚举。逐字对齐 opentdx/const.py（数值不可臆测）。
#pragma once

#include <cstdint>
#include <string_view>

namespace tdx {

// 市场（opentdx const.py:195-199 MARKET）
enum class Market : uint16_t {
  SZ = 0,  // 深圳
  SH = 1,  // 上海
  BJ = 2,  // 北证
};

// K 线周期（opentdx const.py:281-294 PERIOD，逐字移植数值）。
// 注意：MIN_1=7、MINS=8、DAYS=9 与部分文档表述不同，以源码为准。
enum class Period : uint16_t {
  MIN_5     = 0,   // 5 分钟
  MIN_15    = 1,   // 15 分钟
  MIN_30    = 2,   // 30 分钟
  HOUR_1    = 3,   // 60 分钟
  DAILY     = 4,   // 日
  WEEKLY    = 5,   // 周
  MONTHLY   = 6,   // 月
  MIN_1     = 7,   // 1 分钟
  MINS      = 8,   // 多分钟（如 10 分钟）
  DAYS      = 9,   // 多日（如 45 日）
  QUARTERLY = 10,  // 季
  YEARLY    = 11,  // 年
  SECONDS   = 13,  // 多秒（如 5 秒）
};

// 复权（opentdx const.py:296-299 ADJUST）
enum class Adjust : uint8_t {
  NONE = 0,  // 不复权
  QFQ  = 1,  // 前复权
  HFQ  = 2,  // 后复权
};

// A 股分类（opentdx const.py:207-240 CATEGORY，Phase 1 用到的子集）
enum class Category : uint32_t {
  SH        = 0,
  SZ        = 2,
  A         = 6,
  B         = 7,
  KCB       = 8,      // 科创板
  BJ        = 12,     // 北证 A
  CYB       = 14,     // 创业板
  BOARD_ALL = 10000,  // 全部板块
  HGT       = 0x2af9, // 沪股通
  SGT       = 0x2b01, // 深股通
  ETF       = 0x2afd, // ETF 基金
  LOF       = 0x2b04, // LOF 基金
  ZS        = 0x2b2c, // 沪深系列指数
};

// 默认端口（opentdx const.py）
inline constexpr uint16_t kStdPort = 7709;  // 标准行情（A 股）
inline constexpr uint16_t kExPort  = 7727;  // 扩展行情（期货/港美股）
// SP/MAC 高级行情（板块/资金流）走 mac_hosts，同样 7709。

// 时间常量（opentdx，单位秒）
inline constexpr int kHeartbeatIntervalSec = 15;  // 心跳间隔（heartbeat.py DEFAULT_HEARTBEAT_INTERVAL=15.0）
inline constexpr int kHeartbeatMaxIdle     = 20;  // 连续 20 次纯心跳无业务请求则断开
inline constexpr int kConnectTimeoutSec    = 5;   // 连接超时 time_out=5

// 单次请求上限（opentdx）
inline constexpr int kKlineMaxCount = 800;   // K 线单次上限
inline constexpr int kTickMaxCount  = 2000;  // 分笔单次上限

// 价格/金额缩放（opentdx quotationClient.py:24-30）
inline constexpr double kPriceScale  = 100.0;  // 实时行情价格 /100
inline constexpr double kAmountScale = 100.0;  // 金额 *100

// 由股票代码首位推断市场（6/5/9/7 开头 SH，0/2/3 开头 SZ，4/8 开头 BJ）。
inline Market MarketFromCode(std::string_view code) {
  if (code.empty()) return Market::SH;
  char c = code[0];
  if (c == '6' || c == '5' || c == '9' || c == '7') return Market::SH;
  if (c == '0' || c == '2' || c == '3') return Market::SZ;
  if (c == '4' || c == '8') return Market::BJ;
  return Market::SH;  // ETF/可转债等默认 SH
}

}  // namespace tdx
