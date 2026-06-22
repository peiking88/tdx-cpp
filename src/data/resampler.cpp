// Resampler 实现。对齐 tdxdata/sources/base.py:56-132。
#include "tdx/data/resampler.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>

#include "tdx/consts.hpp"
#include "tdx/util/time_util.hpp"

namespace tdx::data {
namespace {

int WeekdayFromEpoch(int64_t epoch) {
  // 0=周日 ... 6=周六（CST epoch → UTC gmtime）
  std::time_t tt = static_cast<std::time_t>(epoch) + 8 * 3600;
  std::tm lt{};
  gmtime_r(&tt, &lt);
  return lt.tm_wday;
}

}  // namespace

int64_t BarEndTimeAShare(int64_t dt_epoch, int period_minutes) {
  // 对齐 base.py:56-76
  auto c = tdx::util::epoch_to_cst(dt_epoch);
  int t = c.hour * 60 + c.minute;
  int elapsed, session_start;
  if (t < tdx::kMorningOpenMin) {
    elapsed = 0;
    session_start = tdx::kMorningOpenMin;
  } else if (t < tdx::kSessionSplitMin) {  // R2: t<720 切分支（非 11:30）
    elapsed = t - tdx::kMorningOpenMin;
    session_start = tdx::kMorningOpenMin;
  } else {
    elapsed = t - tdx::kAfternoonOpenMin;
    session_start = tdx::kAfternoonOpenMin;
  }
  if (elapsed < 0) elapsed = 0;
  int bar_end_min = session_start + (elapsed / period_minutes + 1) * period_minutes;
  return tdx::util::cst_to_epoch(c.year, c.month, c.day, bar_end_min / 60, bar_end_min % 60);
}

namespace {

std::vector<KLine> ResampleMinute(const std::vector<KLine>& kline, int period_min) {
  // base.py:101-132：源周期推断 → effective_start = label - source_min → BarEndTimeAShare
  if (kline.size() < 2) return kline;
  int source_min = 5;
  int64_t delta = kline[1].datetime - kline[0].datetime;
  if (delta > 0) source_min = std::max(1, static_cast<int>(delta / 60));

  // groupby 目标标签（CEIL 标签转换）
  std::map<int64_t, std::vector<const KLine*>> groups;
  for (const auto& k : kline) {
    int64_t effective_start = k.datetime - static_cast<int64_t>(source_min) * 60;
    int64_t label = BarEndTimeAShare(effective_start, period_min);
    groups[label].push_back(&k);
  }

  std::vector<KLine> result;
  for (const auto& [label, ks] : groups) {
    KLine bar;
    bar.datetime = label;
    bar.open = ks.front()->open;
    bar.close = ks.back()->close;
    bar.high = ks.front()->high;
    bar.low = ks.front()->low;
    bar.volume = 0;
    bar.amount = 0;
    for (const auto* k : ks) {
      bar.high = std::max(bar.high, k->high);
      bar.low = std::min(bar.low, k->low);
      bar.volume += k->volume;
      bar.amount += k->amount;
    }
    result.push_back(bar);
  }
  return result;
}

std::vector<KLine> ResampleDaily(const std::vector<KLine>& kline, Freq target) {
  // 周月重采样（base.py:79-98）：groupby 周（周一起点）/月
  std::map<int64_t, std::vector<const KLine*>> groups;  // key = week_start_epoch / month_start_epoch
  for (const auto& k : kline) {
    auto c = tdx::util::epoch_to_cst(k.datetime);
    int64_t day_epoch = tdx::util::cst_to_epoch(c.year, c.month, c.day, 0, 0);
    int64_t key;
    if (target == Freq::Weekly) {
      int wday = WeekdayFromEpoch(k.datetime);  // 0=周日
      int days_since_mon = (wday + 6) % 7;      // 周一=0
      key = day_epoch - static_cast<int64_t>(days_since_mon) * 86400;
    } else {  // Monthly
      key = tdx::util::cst_to_epoch(c.year, c.month, 1, 0, 0);
    }
    groups[key].push_back(&k);
  }

  std::vector<KLine> result;
  for (const auto& [key, ks] : groups) {
    KLine bar;
    bar.datetime = key;
    bar.open = ks.front()->open;
    bar.close = ks.back()->close;
    bar.high = ks.front()->high;
    bar.low = ks.front()->low;
    bar.volume = 0;
    bar.amount = 0;
    for (const auto* k : ks) {
      bar.high = std::max(bar.high, k->high);
      bar.low = std::min(bar.low, k->low);
      bar.volume += k->volume;
      bar.amount += k->amount;
    }
    result.push_back(bar);
  }
  return result;
}

}  // namespace

std::vector<KLine> ResampleKline(const std::vector<KLine>& kline, Freq target) {
  if (kline.empty()) return kline;
  switch (target) {
    case Freq::Min5: return kline;
    case Freq::Min15: return ResampleMinute(kline, 15);
    case Freq::Min30: return ResampleMinute(kline, 30);
    case Freq::Hour1: return ResampleMinute(kline, 60);
    case Freq::Daily: return kline;
    case Freq::Weekly:
    case Freq::Monthly: return ResampleDaily(kline, target);
  }
  return kline;
}

}  // namespace tdx::data
