// tdx 数据结构。datetime 统一为 int64 epoch seconds（CST 时刻对应的绝对 UTC epoch），
// 消除「epoch 或 YYYYMMDD」Dual Schema（架构评审 P2-4）。
// 五档未填档位用 NaN sentinel（评审 P2-3），不用 0（0 会被下游当作有效报价）。
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include "tdx/consts.hpp"

namespace tdx {

// K 线（对齐 opentdx kline.py）。字段顺序 open/close/high/low 反直觉，但与上游逐字一致。
struct KLine {
  int64_t datetime = 0;    // epoch seconds
  double open = 0.0;
  double close = 0.0;      // 注意：close 在 high 之前（移植易错点 D3）
  double high = 0.0;
  double low = 0.0;
  double volume = 0.0;     // 成交量（股）
  double amount = 0.0;     // 成交额（元）
  int32_t up_count = 0;    // 上涨家数（仅指数板块有，否则 0；智能检测 D6）
  int32_t down_count = 0;  // 下跌家数
};

// 分时（对齐 opentdx tick_chart.py）。price/avg 为增量累加后的绝对值（D1）。
struct Tick {
  int64_t datetime = 0;
  double price = 0.0;
  double avg = 0.0;     // 均价
  double volume = 0.0;  // 累计成交量
};

// 买卖方向（对齐 opentdx transaction.py）
enum class BuySell : int8_t {
  Neutral = 0,
  Buy = 1,
  Sell = 2,
};

// 逐笔成交（对齐 opentdx transaction.py）。price 为增量累加后的绝对值（D1）。
struct Transaction {
  int64_t datetime = 0;
  double price = 0.0;
  int64_t volume = 0;    // 成交量（手）
  int64_t trans_id = 0;  // 成交编号
  BuySell buy_sell = BuySell::Neutral;
};

// 五档报价（对齐 opentdx quotes_detail.py）。
// bid/ask 下标 0..4 对应一档..五档；未填档位（盘前）用 NaN（D2 相对基准增量 + P2-3 null 语义）。
struct Quote {
  int64_t datetime = 0;  // 服务器时间 epoch
  std::string code;
  std::string name;       // UTF8（iconv GBK 解码后）
  double price = 0.0;     // 现价（基准价，其他字段相对它增量）
  double pre_close = 0.0;
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double volume = 0.0;    // 总量
  double amount = 0.0;    // 总金额
  std::array<double, 5> bid{};
  std::array<double, 5> ask{};
  std::array<double, 5> bid_vol{};
  std::array<double, 5> ask_vol{};

  Quote() {
    constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
    bid.fill(qnan);
    ask.fill(qnan);
    bid_vol.fill(qnan);
    ask_vol.fill(qnan);
  }
};

// 除权除息事件（对齐 opentdx xdxr；精确字段在 Task 8 从 server.py/client 确认）。
struct Xdxr {
  int64_t datetime = 0;
  double send = 0.0;           // 每股送股
  double dividend = 0.0;       // 每股分红（元）
  double rationed = 0.0;       // 每股配股
  double rationed_price = 0.0; // 配股价
};

// 股票列表项（对齐 opentdx list.py）
struct Stock {
  std::string code;
  std::string name;  // UTF8
  int market = 0;    // Market
  double pre_close = 0.0;
  int decimal_point = 2;
};

}  // namespace tdx
