// A 股时段感知重采样。对齐 tdxdata/sources/base.py:56-132。
// 15m/30m/1h 由 5m 重采样；1w/1mon 由 1d 重采样。
// K 线结束时间按 A 股交易时段（9:30/13:00）标注。
#pragma once

#include <cstdint>
#include <vector>

#include "tdx/types.hpp"

namespace tdx::data {

// 重采样目标频率
enum class Freq { Min1, Min5, Min15, Min30, Hour1, Daily, Weekly, Monthly };

// A 股时段感知的 bar 结束时间（base.py:56-76）。
// 上午 9:30 开盘、下午 13:00 开盘；t<720 切上下午分支。
// 返回 bar 结束时刻的 epoch（同日，hour/minute = bar_end）。
int64_t BarEndTimeAShare(int64_t dt_epoch, int period_minutes);

// 重采样 K 线（base.py:79-132）。
// 分钟：源 5m CEIL 标签 → effective_start = label - source_min → BarEndTimeAShare 算目标标签；
//       groupby 标签聚合（open=first/high=max/low=min/close=last/vol=sum/amount=sum）。
// 周月：由日线 groupby 周（周一）/月。
std::vector<KLine> ResampleKline(const std::vector<KLine>& kline, Freq target);

}  // namespace tdx::data
