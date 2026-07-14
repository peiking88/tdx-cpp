// 字段缩放统一配置。管理不同标的类型 × 不同数据来源的 OHLC/量/额/报价系数。
// 对齐 opentdx quotationClient / daily_bar_reader / tick_chart / transaction 逐字段。
// 纯头文件（inline），proto/ 层直接 include 无链接依赖。
#pragma once

#include <string_view>

#include "tdx/consts.hpp"

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

// 按代码前 2 位 + 市场分类。v0.14.4 新增 Market 参数：解决 00xxxx 在沪市为指数（上证50/180/380
// 等）、在深市为个股（000001 平安银行）的歧义——指数 volume 缩放 ×100（手→股），个股 ×1。
inline SecurityClass ClassifySecurity(std::string_view code, Market market = Market::SH) {
  if (code.size() < 2) return SecurityClass::AStock;
  std::string h2(code.substr(0, 2));
  if (h2 == "88" || h2 == "99") return SecurityClass::Index;
  if (h2 == "39" && code.size() > 2 && code[2] == '9') return SecurityClass::Index;  // 仅 399xxx 为指数，390xxx 非指数
  if (h2 == "00" && market == Market::SH) return SecurityClass::Index;               // 000xxx 沪市=指数
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
    // volume：统一为股（shares）。Index .day raw=手(lots), ×100→股,
    // 新浪实证: tdx×100=sina(全市场成交股数,30天精确). 分钟线无法修正(源数据无真实量).
    case SecurityClass::AStock: s.ohlc = 0.01;  s.volume = 1.0;   break;
    case SecurityClass::Index:  s.ohlc = 0.01;  s.volume = 100.0; break;
    case SecurityClass::SHFund: s.ohlc = 0.001; s.volume = 1.0;   break;
    case SecurityClass::SZFund: s.ohlc = 0.001; s.volume = 1.0;   break;
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
    // 基金报价 3 位小数（对照 Vipdoc1d SHFund/SZFund: ohlc=0.001）；个股/指数 2 位 0.01。
    // 协议 raw 对基金以 0.001 元为单位编码，沿用 0.01 会让基金 OHLC/现价放大 10×。
    switch (sc) {
    case SecurityClass::SHFund:
    case SecurityClass::SZFund:
      s.ohlc = 0.001; s.price = 0.001; s.pre_close = 0.001; s.bid_ask = 0.001;
      break;
    default:  // AStock / Index
      s.ohlc = 0.01; s.price = 0.01; s.pre_close = 0.01; s.bid_ask = 0.01;  // quotationClient.py:36-39
      break;
    }
    s.volume = 100.0;  // 协议返回手，×100 → 股，与 kline 表统一（所有标的一致）
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

// 网络源简化版——固定按 AStock 取系数（sc 不影响结果）。
// 仅限确信无基金标的的 Net* 源使用；NetQuotes（实时报价，含基金）须走双参版，
// 否则基金 OHLC/现价会被当 AStock 放大 10×（基金 0.001 vs 个股 0.01）。Vipdoc* 同理须双参。
inline FieldScaling GetScaling(DataSource src) {
  return GetScaling(SecurityClass::AStock, src);
}

}  // namespace tdx::data
