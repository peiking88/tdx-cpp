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

// 除权除息事件（对齐 opentdx parser/quotation/company_info.py:XDXR，0x0f 响应）。
// 字段名与上游 Python 逐字一致，PerShare 归一由 adjust 模块完成。
struct Xdxr {
  std::string date;            // YYYY-MM-DD
  double fenhong = 0.0;        // 分红（每10股，需 PerShare 归一）
  double peigujia = 0.0;       // 配股价
  double songzhuangu = 0.0;    // 送转股（每10股，需 PerShare 归一）
  double peigu = 0.0;          // 配股（每10股，需 PerShare 归一）
  int category = 1;            // 1=除权除息 2=送配股上市 3=非流通股上市 ...
  std::string name;            // 事件名称（GBK→UTF8）
};

// 股票列表项（对齐 opentdx list.py）
struct Stock {
  std::string code;
  std::string name;  // UTF8
  int market = 0;    // Market
  double pre_close = 0.0;
  int decimal_point = 2;
};

// 扩展行情五档档位（期货/港美股）
struct ExQuoteLevel {
  double price = 0.0;
  double vol = 0.0;
};

// 扩展行情报价（对齐 opentdx help.py:233-287 unpack_futures，核心字段子集）
struct ExQuote {
  uint8_t market = 0;
  std::string code;       // UTF8
  double pre_close = 0.0;
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;
  double vol = 0.0;
  double amount = 0.0;
  double curr_vol = 0.0;
  uint32_t open_position = 0;
  uint32_t add_position = 0;
  uint32_t hold_position = 0;
  std::array<ExQuoteLevel, 5> bid{};
  std::array<ExQuoteLevel, 5> ask{};
  double settlement = 0.0;
  double avg = 0.0;
  double pre_settlement = 0.0;
  int64_t datetime = 0;  // date_raw → epoch（仅日期）
};

// ---- 财务数据 0x10（对齐 company_info.py:Finance） ----
struct Finance {
  std::string code;
  double liutongguben = 0.0;   // 流通股本（万股）
  int province = 0;
  int industry = 0;            // 行业代码
  uint32_t updated_date = 0;   // YYYYMMDD
  uint32_t ipo_date = 0;       // 上市日期
  double zongguben = 0.0;      // 总股本
  double guojiagu = 0.0;
  double faqirenfarengu = 0.0;
  double farengu = 0.0;
  double bgu = 0.0;
  double hgu = 0.0;
  double meigushouyi = 0.0;    // 每股收益
  double zichanzongji = 0.0;
  double liudongzichanzongji = 0.0;
  double gudingzichanjine = 0.0;
  double wuxingzichan = 0.0;
  double gudongrenshu = 0.0;
  double liudongfuzhaiheji = 0.0;
  double changqifuzhai = 0.0;
  double zibengongjijin = 0.0;
  double guimuquanyineji = 0.0;
  double yinyezongshouru = 0.0;
  double yinyechengben = 0.0;
  double yingshouzhanngkuan = 0.0;
  double yinyelirun = 0.0;
  double touzishouyi = 0.0;
  double jingyinxianjinliujine = 0.0;
  double zongxianjinliu = 0.0;
  double cunhuo = 0.0;
  double lirunzonge = 0.0;
  double shuihoulirun = 0.0;
  double guimujinlirun = 0.0;
  double weifenlirun = 0.0;
  double meigujinzichan = 0.0;
};

// ---- F10 分类目录 0x2cf（对齐 company_info.py:Category） ----
struct F10Category {
  std::string name;       // 分类名 (GBK→UTF8)
  std::string filename;   // 文件名 (GBK→UTF8)
  uint32_t start = 0;
  uint32_t length = 0;
};

// ---- F10 内容 0x2d0（对齐 company_info.py:Content） ----
struct F10Content {
  int market = 0;
  std::string code;
  int category = 0;
  uint32_t length = 0;
  std::string content;    // GBK 文本
};

// ---- 历史委托 0xfb4（对齐 history_orders.py） ----
struct HistoryOrder {
  double price = 0.0;     // 增量累加价格
  int64_t unknown = 0;
  int64_t vol = 0;
};

// ---- 历史逐笔 0xfb5（对齐 history_transaction.py） ----
struct HistoryTransaction {
  int minutes = 0;        // 当日分钟数
  double price = 0.0;     // 增量累加价格
  int64_t vol = 0;
  int buy_sell = 0;       // 0=BUY 1=SELL 2=NEUTRAL
};

// ---- 成交量分布档 0x51a（对齐 volume_profile.py） ----
struct QuoteLevel {
  double price = 0.0;
  double vol = 0.0;
};

struct VolProfileLevel {
  double price = 0.0;
  int64_t vol = 0;
  int64_t buy = 0;
  int64_t sell = 0;
};

struct VolProfile {
  int market = 0;
  std::string code;
  double price = 0.0;
  double pre_close = 0.0;
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double vol = 0.0;
  double amount = 0.0;
  std::vector<QuoteLevel> handicap_bid;
  std::vector<QuoteLevel> handicap_ask;
  std::vector<VolProfileLevel> levels;
};

// ---- 指数信息 0x51d（对齐 index_info.py） ----
struct IndexOrder {
  int64_t price = 0;
  int64_t unknown = 0;
  int64_t vol = 0;
};

struct IndexInfo {
  int market = 0;
  std::string code;
  double close = 0.0;
  double pre_close = 0.0;
  double diff = 0.0;       // pre_close - close
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double vol = 0.0;
  double amount = 0.0;
  int64_t up_count = 0;
  int64_t down_count = 0;
  std::vector<IndexOrder> orders;
};

// ---- 主力异动 0x563（对齐 unusual.py） ----
struct UnusualItem {
  int index = 0;
  int market = 0;
  std::string code;
  int hour = 0;
  int minute = 0;
  int second = 0;
  std::string desc;        // 异动类型描述（中文）
  std::string value_str;   // 格式化数值（如 "3.50%/2.00%"）
  double value = 0.0;      // 数值（用于排序/过滤）
  int unusual_type = 0;
};

// ---- 板块列表 0x1231（对齐 board_list.py，已有 SpBoard） ----
struct BoardItem {
  int market = 0;
  std::string code;
  std::string name;
  double price = 0.0;
  double rise_speed = 0.0;
  double pre_close = 0.0;
};

// ---- 资金流向 0x1218（对齐 symbol_capital_flow.py） ----
struct CapitalFlow {
  std::string symbol;
  int market = 0;
  double main_in = 0.0;      // 今日主力流入
  double main_out = 0.0;     // 今日主力流出
  double retail_in = 0.0;    // 今日散户流入
  double retail_out = 0.0;   // 今日散户流出
  double d5_main_buy = 0.0;  // 5日主买
  double d5_main_sell = 0.0; // 5日主卖
};

}  // namespace tdx
