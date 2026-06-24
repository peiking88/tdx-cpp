// 复权因子自算。对齐 tdxdata/sources/adjust.py:49-214。
// 基于 xdxr 事件流自算前/后复权因子（不是直接取交易所因子）。
//   qfq（前复权）：新→旧遍历，event_factor=numerator/denominator，末尾除以最新因子（基准归一）
//   hfq（后复权）：旧→新遍历，event_factor=denominator/numerator，累计累积
#pragma once

#include <string>
#include <vector>

#include "tdx/types.hpp"

namespace tdx::data {

enum class AdjustType { None, Qfq, Hfq };

// 复权因子点
struct FactorPoint {
  std::string date;   // YYYY-MM-DD
  double factor = 1.0;
};

// _per_share 归一（adjust.py:149-151）：value>=1 视作每 10 股 → /10
double PerShare(double value);

// 从 xdxr 事件流自算复权因子（adjust.py:49-104）
std::vector<FactorPoint> ComputeFactorFromXdxr(const std::vector<Xdxr>& xdxr,
                                               const std::vector<KLine>& kline,
                                               AdjustType adjust);

// 应用复权因子到 K 线（adjust.py:154-214）
// qfq: backward-asof + 末尾归一；hfq: forward-asof。仅 OHLC 乘因子，vol/amount 不变。
void ApplyAdjust(std::vector<KLine>& kline, const std::vector<FactorPoint>& factors,
                 AdjustType adjust);

}  // namespace tdx::data
