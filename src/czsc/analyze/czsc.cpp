// CZSC 缠论分析器实现
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/analyze/mod.rs:92-305
#include "czsc/analyze/czsc.hpp"
#include "czsc/analyze/algorithms.hpp"

#include <algorithm>
#include <cassert>
#include <string>

namespace czsc::analyze {

// ============================================================
// CZSC::new — 批量初始化（mod.rs:125-145）
// ============================================================
CZSC::CZSC(const std::vector<RawBar>& bars, size_t max_bi, size_t min_bi)
    : max_bi_num(max_bi), min_bi_len(min_bi) {
  if (bars.empty()) return;

  symbol = bars[0].symbol;
  freq = bars[0].freq;

  for (const auto& bar : bars)
    update_bar(bar);
}

// ============================================================
// sync_extended_last_ubi_in_bis（mod.rs:95-123）
// ============================================================
void CZSC::sync_extended_last_ubi_in_bis(const NewBar& last_ubi, const RawBar& bar) {
  // 辅助：在 NewBar 中查找匹配并替换最后一个 elements
  auto patch_new_bar = [&](NewBar& nb) {
    if (nb == last_ubi && !nb.elements.empty() && nb.elements.back().dt == bar.dt) {
      nb.elements.back() = bar;
    }
  };

  // 辅助：在 FX 中查找匹配
  auto patch_fx = [&](FX& fx) {
    for (auto& nb : fx.elements)
      patch_new_bar(nb);
  };

  // 遍历 bi_list 中的所有 NewBar 和 FX
  for (auto& bi : bi_list) {
    for (auto& nb : bi.bars) patch_new_bar(nb);
    patch_fx(bi.fx_a);
    patch_fx(bi.fx_b);
    for (auto& fx : bi.fxs) patch_fx(fx);
  }
}

// ============================================================
// update_bar（mod.rs:167-228）
// ============================================================
void CZSC::update_bar(const RawBar& bar) {
  // Step 1: 处理 bars_raw 的 push / 同 dt 替换
  std::vector<RawBar> last_bars;
  if (bars_raw.empty() || bar.dt != bars_raw.back().dt) {
    bars_raw.push_back(bar);
    last_bars = {bar};
  } else {
    // 同 dt 延伸（mod.rs:173-188）
    *bars_raw.rbegin() = bar;

    if (!bars_ubi.empty()) {
      auto last_ubi = bars_ubi.back();
      bars_ubi.pop_back();
      sync_extended_last_ubi_in_bis(last_ubi, bar);

      last_bars = last_ubi.elements;
      assert(!last_bars.empty() && bar.dt == last_bars.back().dt);
      last_bars.back() = bar;
    } else {
      last_bars = {bar};
    }
  }

  // Step 2: 去除包含关系（mod.rs:191-209）
  for (const auto& b : last_bars) {
    if (bars_ubi.size() < 2) {
      bars_ubi.push_back(NewBar::FromRaw(b));
    } else {
      auto& k1 = bars_ubi[bars_ubi.size() - 2];
      auto& k2 = bars_ubi[bars_ubi.size() - 1];
      auto [has_include, k4] = remove_include(k1, k2, b);
      if (has_include) {
        bars_ubi.back() = k4;
      } else {
        bars_ubi.push_back(k4);
      }
    }
  }

  // Step 3: 更新笔（mod.rs:211-213）
  update_bi_();

  // Step 4: 笔数量限制（mod.rs:214-217）
  if (bi_list.size() > max_bi_num) {
    size_t drain_to = bi_list.size() - max_bi_num;
    bi_list.erase(bi_list.begin(), bi_list.begin() + drain_to);
  }

  // Step 5: bars_raw drain（mod.rs:219-224）
  if (!bi_list.empty()) {
    int64_t sdt = bi_list.front().fx_a.elements[0].dt;
    auto it = std::lower_bound(bars_raw.begin(), bars_raw.end(), sdt,
                               [](const RawBar& b, int64_t dt) { return b.dt < dt; });
    bars_raw.erase(bars_raw.begin(), it);
  }
}

// ============================================================
// update_bi_（mod.rs:230-305）
// ============================================================
void CZSC::find_first_bi_() {
  // 首次笔查找（mod.rs:237-263）
  auto fxs = check_fxs(bars_ubi);
  if (fxs.empty()) return;

  const FX* first = &fxs[0];
  const FX* fx_a = first;
  if (first->mark == Mark::kD) {
    // 底分型：取最低的那个
    for (auto& x : fxs)
      if (x.mark == Mark::kD && x.low <= fx_a->low) fx_a = &x;
  } else {
    // 顶分型：取最高的那个
    for (auto& x : fxs)
      if (x.mark == Mark::kG && x.high >= fx_a->high) fx_a = &x;
  }

  // 从 fx_a 第一根 K 线开始截取 bars_ubi
  int64_t fx_start = fx_a->elements[0].dt;
  std::vector<NewBar> after_start;
  for (auto& b : bars_ubi)
    if (b.dt >= fx_start) after_start.push_back(b);

  auto [bi, rest_start] = check_bi(after_start, min_bi_len);
  if (bi) bi_list.push_back(*bi);

  // 更新 bars_ubi
  bars_ubi.clear();
  for (size_t i = rest_start; i < after_start.size(); ++i)
    bars_ubi.push_back(after_start[i]);
}

void CZSC::incremental_bi_find_() {
  // 增量笔查找（mod.rs:273-278）
  auto [bi, rest_start] = check_bi(bars_ubi, min_bi_len);
  if (bi) bi_list.push_back(*bi);

  std::vector<NewBar> rest;
  for (size_t i = rest_start; i < bars_ubi.size(); ++i)
    rest.push_back(bars_ubi[i]);
  bars_ubi = rest;
}

bool CZSC::last_bi_is_broken_() const {
  // 后处理：笔破坏检测（mod.rs:281-304）
  if (bars_ubi.empty() || bi_list.empty()) return false;

  const auto& last_bi = bi_list.back();
  if (last_bi.direction == Direction::kUp) {
    // 向上笔被破坏：bars_ubi 中有 higher high
    double max_high = bars_ubi.back().high;
    for (auto& b : bars_ubi)
      if (b.high > max_high) max_high = b.high;
    return max_high > last_bi.get_high();
  } else {
    // 向下笔被破坏：bars_ubi 中有 lower low
    double min_low = bars_ubi.back().low;
    for (auto& b : bars_ubi)
      if (b.low < min_low) min_low = b.low;
    return min_low < last_bi.get_low();
  }
}

void CZSC::handle_bi_break_() {
  // 笔被破坏——merge 并 pop（mod.rs:299-304）
  const auto& last_bi = bi_list.back();
  if (last_bi.bars.size() < 2) return;

  int64_t merge_point = last_bi.bars[last_bi.bars.size() - 2].dt;
  std::vector<NewBar> merged;
  // last_bi->bars[0..size-2]
  for (size_t i = 0; i + 2 < last_bi.bars.size(); ++i)
    merged.push_back(last_bi.bars[i]);
  // bars_ubi 中 dt >= merge_point 的
  for (auto& b : bars_ubi)
    if (b.dt >= merge_point) merged.push_back(b);

  bars_ubi = merged;
  bi_list.pop_back();
}

void CZSC::update_bi_() {
  if (bars_ubi.size() < 3) return;
  if (bi_list.empty()) {
    find_first_bi_();
    return;
  }
  incremental_bi_find_();
  if (last_bi_is_broken_()) handle_bi_break_();
}

// ============================================================
// get_fx_list（mod.rs:148-162）
// ============================================================
std::vector<FX> CZSC::get_fx_list() const {
  std::vector<FX> fxs;
  for (auto& bi : bi_list) {
    // bi.fxs[1..]
    for (size_t i = 1; i < bi.fxs.size(); ++i)
      fxs.push_back(bi.fxs[i]);
  }

  auto ubi_fxs = get_ubi_fxs();
  for (auto& x : ubi_fxs) {
    if (fxs.empty() || x.dt > fxs.back().dt)
      fxs.push_back(x);
  }
  return fxs;
}

// ============================================================
// get_ubi_fxs（mod.rs:308-313）
// ============================================================
std::vector<FX> CZSC::get_ubi_fxs() const {
  if (bars_ubi.empty()) return {};
  return check_fxs(bars_ubi);
}

}  // namespace czsc::analyze
