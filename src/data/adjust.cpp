// Adjust 实现。对齐 tdxdata/sources/adjust.py:49-214。
#include "tdx/data/adjust.hpp"

#include <algorithm>
#include <cstdio>

#include "tdx/util/time_util.hpp"

namespace tdx::data {
namespace {

int64_t DateToEpoch(const std::string& date) {
  if (date.size() < 10) return 0;
  int y = std::stoi(date.substr(0, 4));
  int m = std::stoi(date.substr(5, 2));
  int d = std::stoi(date.substr(8, 2));
  return tdx::util::date_to_epoch(y, m, d);
}

std::string EpochToDate(int64_t epoch) {
  auto c = tdx::util::epoch_to_cst(epoch);
  char b[16];
  std::snprintf(b, sizeof(b), "%04d-%02d-%02d", c.year, c.month, c.day);
  return std::string(b);
}

}  // namespace

double PerShare(double value) {
  // adjust.py:149-151
  return value >= 1.0 ? value / 10.0 : value;
}

std::vector<FactorPoint> ComputeFactorFromXdxr(const std::vector<XdxrEvent>& xdxr,
                                               const std::vector<KLine>& kline,
                                               AdjustType adjust) {
  std::vector<FactorPoint> result;
  if (xdxr.empty() || kline.empty() || adjust == AdjustType::None) return result;

  // 排序事件：qfq 新→旧(desc)，hfq 旧→新(asc)
  auto events = xdxr;
  bool qfq = (adjust == AdjustType::Qfq);
  std::sort(events.begin(), events.end(),
            [qfq](const XdxrEvent& a, const XdxrEvent& b) {
              return qfq ? a.date > b.date : a.date < b.date;
            });

  // K 线按 datetime 升序（找 pre_close）
  auto sorted_kline = kline;
  std::sort(sorted_kline.begin(), sorted_kline.end(),
            [](const KLine& a, const KLine& b) { return a.datetime < b.datetime; });

  double cumulative = 1.0;
  for (const auto& event : events) {
    int64_t event_epoch = DateToEpoch(event.date);
    // pre_close = kline 中 datetime < event_epoch 的最后一根 close
    double pre_close = 0.0;
    for (const auto& k : sorted_kline) {
      if (k.datetime < event_epoch) pre_close = k.close;
      else break;
    }

    FactorPoint fp;
    fp.date = event.date;
    if (pre_close == 0.0) {
      fp.factor = cumulative;  // 无前收盘价，保持累计
    } else {
      double fenhong = PerShare(event.fenhong);
      double peigujia = event.peigujia;
      double songzhuangu = PerShare(event.songzhuangu);
      double peigu = PerShare(event.peigu);
      double event_factor = 1.0;
      // category in {1,2} 或 name=="除权除息" 才算因子
      if (event.category == 1 || event.category == 2 || event.name == "除权除息") {
        double numerator = pre_close - fenhong + peigujia * peigu;
        double denominator = pre_close * (1.0 + songzhuangu + peigu);
        if (denominator == 0.0 || numerator == 0.0) {
          event_factor = 1.0;
        } else if (qfq) {
          event_factor = numerator / denominator;
        } else {
          event_factor = denominator / numerator;
        }
      }
      cumulative *= event_factor;
      fp.factor = cumulative;
    }
    result.push_back(fp);
  }
  return result;
}

void ApplyAdjust(std::vector<KLine>& kline, const std::vector<FactorPoint>& factors,
                 AdjustType adjust) {
  if (factors.empty() || kline.empty() || adjust == AdjustType::None) return;

  // 因子按 date 升序
  auto fac = factors;
  std::sort(fac.begin(), fac.end(),
            [](const FactorPoint& a, const FactorPoint& b) { return a.date < b.date; });

  // qfq 末尾归一（除以最新因子，使最新日 factor=1，adjust.py:204-207）
  if (adjust == AdjustType::Qfq) {
    double latest = fac.back().factor;
    if (latest > 0) {
      for (auto& f : fac) f.factor /= latest;
    }
  }

  for (auto& k : kline) {
    std::string kdate = EpochToDate(k.datetime);
    double factor = 1.0;
    if (adjust == AdjustType::Qfq) {
      // backward-asof：找 date <= kdate 的最大因子
      for (const auto& f : fac) {
        if (f.date <= kdate) factor = f.factor;
        else break;
      }
    } else {
      // forward-asof：找 date >= kdate 的最小因子
      for (const auto& f : fac) {
        if (f.date >= kdate) { factor = f.factor; break; }
      }
    }
    // 仅 OHLC 乘因子，vol/amount 不变（adjust.py:209-211）
    k.open *= factor;
    k.close *= factor;
    k.high *= factor;
    k.low *= factor;
  }
}

}  // namespace tdx::data
