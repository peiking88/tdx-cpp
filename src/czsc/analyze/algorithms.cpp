// 缠论分析算法实现
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/analyze/utils.rs:32-298
#include "czsc/analyze/algorithms.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>

namespace czsc::analyze {

// ============================================================
// remove_include（utils.rs:32-103）
// ============================================================
RemoveIncludeResult remove_include(const NewBar& k1, const NewBar& k2, const RawBar& k3) {
  // Step 1: 方向判断
  Direction direction;
  if (k1.high < k2.high)
    direction = Direction::kUp;
  else if (k1.high > k2.high)
    direction = Direction::kDown;
  else
    return {false, NewBar::FromRaw(k3)};  // k1.high == k2.high → 直接返回 k3

  // Step 2: 包含关系判定
  bool has_inclusion =
    (k2.high <= k3.high && k2.low >= k3.low) ||
    (k2.high >= k3.high && k2.low <= k3.low);

  if (!has_inclusion)
    return {false, NewBar::FromRaw(k3)};

  // Step 3: 处理后包含K线的高低点和时间
  double high, low;
  int64_t dt;
  if (direction == Direction::kUp) {
    high = std::max(k2.high, k3.high);
    low  = std::max(k2.low, k3.low);
    dt   = (k2.high > k3.high) ? k2.dt : k3.dt;
  } else {
    high = std::min(k2.high, k3.high);
    low  = std::min(k2.low, k3.low);
    dt   = (k2.low < k3.low) ? k2.dt : k3.dt;
  }

  // Step 4: 根据 k3 阴阳决定 open/close
  double open_, close;
  if (k3.open > k3.close) { open_ = high; close = low; }
  else                     { open_ = low;  close = high; }

  // Step 5: elements = k2.elements (去 k3.dt 的) + k3
  NewBar k4;
  k4.symbol = k2.symbol;
  k4.id = k2.id;
  k4.freq = k2.freq;
  k4.dt = dt;
  k4.open = open_;
  k4.close = close;
  k4.high = high;
  k4.low = low;
  k4.vol = k2.vol + k3.vol;
  k4.amount = k2.amount + k3.amount;

  // 保留 k2.elements 中 dt != k3.dt 的条目，最多 100 个
  int kept = 0;
  for (const auto& e : k2.elements) {
    if (e.dt != k3.dt && kept < 100) {
      k4.elements.push_back(e);
      ++kept;
    }
  }
  k4.elements.push_back(k3);

  return {true, k4};
}

// ============================================================
// check_fx（utils.rs:158-192）
// ============================================================
std::optional<FX> check_fx(const NewBar& k1, const NewBar& k2, const NewBar& k3) {
  // 顶分型
  if (k1.high < k2.high && k2.high > k3.high &&
      k1.low < k2.low && k2.low > k3.low) {
    FX fx;
    fx.symbol = k1.symbol;
    fx.dt = k2.dt;
    fx.mark = Mark::kG;
    fx.high = k2.high;
    fx.low = k2.low;
    fx.fx = k2.high;
    fx.elements = {k1, k2, k3};
    return fx;
  }

  // 底分型
  if (k1.low > k2.low && k2.low < k3.low &&
      k1.high > k2.high && k2.high < k3.high) {
    FX fx;
    fx.symbol = k1.symbol;
    fx.dt = k2.dt;
    fx.mark = Mark::kD;
    fx.high = k2.high;
    fx.low = k2.low;
    fx.fx = k2.low;
    fx.elements = {k1, k2, k3};
    return fx;
  }

  return std::nullopt;
}

// ============================================================
// check_fxs（utils.rs:119-140）
// ============================================================
std::vector<FX> check_fxs(const std::vector<NewBar>& bars_ubi) {
  std::vector<FX> fxs;
  for (size_t i = 0; i + 2 < bars_ubi.size(); ++i) {
    auto fx1 = check_fx(bars_ubi[i], bars_ubi[i + 1], bars_ubi[i + 2]);
    if (fx1) {
      // 过滤连续同标记分型
      if (fxs.size() >= 2 && fx1->mark == fxs.back().mark) {
        std::cerr << "check_fxs: consecutive same mark at dt="
                  << bars_ubi[i + 1].dt << std::endl;
      } else {
        fxs.push_back(*fx1);
      }
    }
  }
  return fxs;
}

// ============================================================
// check_bi（utils.rs:198-298）
// ============================================================
CheckBiResult check_bi(const std::vector<NewBar>& bars, size_t min_bi_len) {
  auto fxs = check_fxs(bars);
  if (fxs.size() < 2)
    return {std::nullopt, 0};

  const FX* fx_a = &fxs[0];
  Direction direction;
  const FX* fx_b = nullptr;

  if (fx_a->mark == Mark::kD) {
    // 找底分型后的顶分型（utils.rs:209-224）
    for (auto& x : fxs) {
      if (x.mark == Mark::kG && x.dt > fx_a->dt && x.fx > fx_a->fx) {
        if (!fx_b || x.high > fx_b->high) fx_b = &x;
      }
    }
    direction = Direction::kUp;
  } else {
    // 找顶分型后的底分型（utils.rs:226-242）
    for (auto& x : fxs) {
      if (x.mark == Mark::kD && x.dt > fx_a->dt && x.fx < fx_a->fx) {
        if (!fx_b || x.low < fx_b->low) fx_b = &x;
      }
    }
    direction = Direction::kDown;
  }

  if (!fx_b) return {std::nullopt, 0};

  // partition_point 确定 bars_a 区间
  int64_t start_dt = fx_a->elements[0].dt;
  int64_t end_dt   = fx_b->elements[2].dt;

  size_t start_idx = 0;
  while (start_idx < bars.size() && bars[start_idx].dt < start_dt) ++start_idx;
  size_t end_idx = start_idx;
  while (end_idx < bars.size() && bars[end_idx].dt <= end_dt) ++end_idx;

  if (start_idx >= end_idx) return {std::nullopt, 0};

  // 成笔条件判定
  bool ab_include = (fx_a->high > fx_b->high && fx_a->low < fx_b->low) ||
                    (fx_a->high < fx_b->high && fx_a->low > fx_b->low);

  if (!ab_include && (end_idx - start_idx) >= min_bi_len) {
    // 构建 BI
    BI bi;
    bi.symbol = fx_a->symbol;
    bi.fx_a = *fx_a;
    bi.fx_b = *fx_b;
    bi.direction = direction;

    // 过滤 fxs 到 [start_dt, end_dt]
    for (auto& fx : fxs) {
      if (fx.dt >= start_dt && fx.dt <= end_dt)
        bi.fxs.push_back(fx);
    }

    // bars_a
    for (size_t i = start_idx; i < end_idx; ++i)
      bi.bars.push_back(bars[i]);

    // new_start_dt = fx_b.elements[0].dt
    int64_t new_start_dt = fx_b->elements[0].dt;
    size_t new_start_idx = start_idx;
    while (new_start_idx < bars.size() && bars[new_start_idx].dt < new_start_dt) ++new_start_idx;

    return {bi, new_start_idx};
  }

  return {std::nullopt, 0};
}

// ---- 一类买卖点：趋势背驰判定（utils/cxt.rs:219-335）----
// 说明：价格首尾笔同向、首笔极值占据端点、末笔创新低(高)但力度(power_price/volume/length)
//       低于关键笔与前一笔力度的均值 → 背驰，构成一买(一卖)。

namespace {

inline double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// 从奇数下标(1,3,...)笔中，按"低点递降"筛选关键笔序列
// 对齐 utils/cxt.rs check_first_buy: for i in (0..=len-3).step_by(2)
std::vector<std::reference_wrapper<const BI>> key_bis_down(const std::vector<BI>& bis) {
  std::vector<std::reference_wrapper<const BI>> key;
  for (size_t i = 0; i + 2 < bis.size(); i += 2) {
    if (i == 0) {
      key.emplace_back(bis[i]);
    } else {
      const BI& b1 = bis[i - 2];
      const BI& b3 = bis[i];
      if (b3.get_low() < b1.get_low()) key.emplace_back(b3);
    }
  }
  return key;
}

// 从奇数下标(1,3,...)笔中，按"高点递升"筛选关键笔序列（一卖用）
std::vector<std::reference_wrapper<const BI>> key_bis_up(const std::vector<BI>& bis) {
  std::vector<std::reference_wrapper<const BI>> key;
  for (size_t i = 0; i + 2 < bis.size(); i += 2) {
    if (i == 0) {
      key.emplace_back(bis[i]);
    } else {
      const BI& b1 = bis[i - 2];
      const BI& b3 = bis[i];
      if (b3.get_high() > b1.get_high()) key.emplace_back(b3);
    }
  }
  return key;
}

// 由关键笔序列计算三类力度的均值
void power_means(const std::vector<std::reference_wrapper<const BI>>& key,
                  double& mp, double& mv, double& ml) {
  std::vector<double> pp, pv, pl;
  pp.reserve(key.size()); pv.reserve(key.size()); pl.reserve(key.size());
  for (const BI& bi : key) {
    pp.push_back(bi.get_power_price());
    pv.push_back(bi.get_power_volume());
    pl.push_back(static_cast<double>(bi.get_length()));
  }
  mp = Mean(pp); mv = Mean(pv); ml = Mean(pl);
}

}  // namespace

bool check_first_buy(const std::vector<BI>& bis) {
  // 1. 奇数笔、末笔向下、首笔=末笔方向
  if (bis.size() % 2 != 1 || bis.back().direction == Direction::kUp) return false;
  if (bis.front().direction != bis.back().direction) return false;
  // 2. 首笔最高、末笔最低
  double max_high = -std::numeric_limits<double>::infinity();
  double min_low = std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) {
    max_high = std::max(max_high, bi.get_high());
    min_low = std::min(min_low, bi.get_low());
  }
  if (max_high != bis.front().get_high() || min_low != bis.back().get_low()) return false;
  // 3. 关键笔序列（低点递降的偶位笔）
  auto key = key_bis_down(bis);
  if (key.empty()) return false;
  // 4. 背驰：末笔力度 < prev与key均值 的最大值
  const BI& last = bis.back();
  const BI& prev = bis[bis.size() - 3];
  double kp, kv, kl;
  power_means(key, kp, kv, kl);
  const bool bc_price = last.get_power_price() < std::max(prev.get_power_price(), kp);
  const bool bc_volume = last.get_power_volume() < std::max(prev.get_power_volume(), kv);
  const bool bc_length = static_cast<double>(last.get_length()) <
                         std::max(static_cast<double>(prev.get_length()), kl);
  return bc_price && (bc_volume || bc_length);
}

bool check_first_sell(const std::vector<BI>& bis) {
  // 反向：奇数笔、末笔向上、首笔最低、末笔最高、新高但力度背离
  if (bis.size() % 2 != 1 || bis.back().direction == Direction::kDown) return false;
  if (bis.front().direction != bis.back().direction) return false;
  double max_high = -std::numeric_limits<double>::infinity();
  double min_low = std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) {
    max_high = std::max(max_high, bi.get_high());
    min_low = std::min(min_low, bi.get_low());
  }
  if (max_high != bis.back().get_high() || min_low != bis.front().get_low()) return false;
  auto key = key_bis_up(bis);
  if (key.empty()) return false;
  const BI& last = bis.back();
  const BI& prev = bis[bis.size() - 3];
  double kp, kv, kl;
  power_means(key, kp, kv, kl);
  const bool bc_price = last.get_power_price() < std::max(prev.get_power_price(), kp);
  const bool bc_volume = last.get_power_volume() < std::max(prev.get_power_volume(), kv);
  const bool bc_length = static_cast<double>(last.get_length()) <
                         std::max(static_cast<double>(prev.get_length()), kl);
  return bc_price && (bc_volume || bc_length);
}

}  // namespace czsc::analyze
