// 字段缩放统一配置。管理不同标的类型 × 不同数据来源的 OHLC/量/额/报价系数。
// 对齐 opentdx quotationClient / daily_bar_reader / tick_chart / transaction 逐字段。
// 纯头文件（inline），proto/ 层直接 include 无链接依赖。
#pragma once

#include <string_view>

namespace tdx::data {

enum class SecurityClass { AStock, Index, SHFund, SZFund };

enum class DataSource {
  Vipdoc1d,        // 本地 .day 日线
  VipdocMin,       // 本地 .lc1/.lc5 分钟线 (float 直读)
  NetKlineStd,     // 网络 0x523 A股K线
  NetKlineEx,      // 网络 0x23ff/0x2489 扩展行情K线
  NetKlineSp,      // 网络 0x122E SP板块K线
  NetQuotes,       // 网络 0x53e 五档报价
  NetTick,         // 网络 0x537 分时
  NetTransaction,  // 网络 0xfc5 逐笔
  NetList,         // 网络 0x44d 股票列表
};

struct FieldScaling {
  double ohlc      = 1.0;  // K线/报价 open/high/low/close
  double volume    = 1.0;  // 成交量
  double amount    = 1.0;  // 成交额
  double price     = 1.0;  // 现价（分时/逐笔/报价基准）
  double pre_close = 1.0;  // 昨收（报价）
  double bid_ask   = 1.0;  // 五档买卖价（报价）
  double avg       = 1.0;  // 均价（分时）
};

// 仅凭前 2 位代码分类，不带 Market 参数。
// 前提：50/51/58 仅存在于沪市 vipdoc（SH_FUND），15/16/18 仅存在于深市 vipdoc（SZ_FUND），
// 无跨市场歧义。若未来导入 SH 债券（15/16/18?xxxx）需加 Market 参数区分 SZFund vs SHBond。
inline SecurityClass ClassifySecurity(std::string_view code) {
  if (code.size() < 2) return SecurityClass::AStock;
  std::string h2(code.substr(0, 2));
  if (h2 == "88" || h2 == "99") return SecurityClass::Index;
  if (h2 == "39")              return SecurityClass::Index;
  if (h2 == "50" || h2 == "51" || h2 == "58") return SecurityClass::SHFund;
  if (h2 == "15" || h2 == "16" || h2 == "18") return SecurityClass::SZFund;
  return SecurityClass::AStock;
}

inline FieldScaling GetScaling(SecurityClass sc, DataSource src) {
  FieldScaling s;
  switch (src) {
  case DataSource::Vipdoc1d:
    s.amount = 1.0;
    switch (sc) {
    case SecurityClass::AStock: s.ohlc = 0.01;  s.volume = 0.01;  break;
    case SecurityClass::Index:  s.ohlc = 0.01;  s.volume = 1.0;   break;
    case SecurityClass::SHFund: s.ohlc = 0.001; s.volume = 1.0;   break;
    case SecurityClass::SZFund: s.ohlc = 0.001; s.volume = 0.01;  break;
    }
    break;
  case DataSource::VipdocMin:
    break;
  case DataSource::NetKlineStd:
    s.ohlc = 0.001;     // quotationClient.py:133-136
    break;
  case DataSource::NetKlineEx:
  case DataSource::NetKlineSp:
    break;
  case DataSource::NetQuotes:
    s.ohlc      = 0.01;   // quotationClient.py:36-39
    s.price     = 0.01;
    s.pre_close = 0.01;
    s.bid_ask   = 0.01;
    // ponytail: amount/open_amount 不缩放——Python 只对 open_amount *= 100，C++ 侧 open_amount 未保留
    break;
  case DataSource::NetTick:
    s.price = 0.01;       // quotationClient.py:150
    s.avg   = 0.0001;     // quotationClient.py:151
    break;
  case DataSource::NetTransaction:
    s.price = 0.01;       // quotationClient.py:199
    break;
  case DataSource::NetList:
    break;
  }
  return s;
}

// 网络源简化版——所有标的统一编码，sc 不影响结果。
// 仅限 Net* 源使用；Vipdoc* 源必须走双参版，否则 SHFund/SZFund 系数错误。
inline FieldScaling GetScaling(DataSource src) {
  return GetScaling(SecurityClass::AStock, src);
}

}  // namespace tdx::data
