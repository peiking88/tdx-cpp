// 缠论分析算法函数声明
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/analyze/utils.rs:32-298
#pragma once

#include <optional>
#include <vector>

#include "czsc/types/bi.hpp"
#include "czsc/types/fx.hpp"
#include "czsc/types/new_bar.hpp"
#include "czsc/types/raw_bar.hpp"

namespace czsc::analyze {

// ---- remove_include — 包含关系处理（utils.rs:32-103）----

struct RemoveIncludeResult {
  bool has_include;
  NewBar k4;
};
RemoveIncludeResult remove_include(const NewBar& k1, const NewBar& k2, const RawBar& k3);

// ---- check_fx — 单分型检测（utils.rs:158-192）----
std::optional<FX> check_fx(const NewBar& k1, const NewBar& k2, const NewBar& k3);

// ---- check_fxs — 全分型扫描（utils.rs:119-140）----
std::vector<FX> check_fxs(const std::vector<NewBar>& bars_ubi);

// ---- check_bi — 笔检测（utils.rs:198-298）----
struct CheckBiResult {
  std::optional<BI> bi;
  size_t rest_start;  // 输入 bars 中剩余部分的起始下标
};
CheckBiResult check_bi(const std::vector<NewBar>& bars, size_t min_bi_len);

// ---- check_first_buy/sell — 一类买卖点：趋势背驰判定（utils/cxt.rs:219-335）----
// 一买：奇数笔、首末笔均向下、首笔最高末笔最低、价格新低但力度背离 → true
bool check_first_buy(const std::vector<BI>& bis);
// 一卖：同上反向（首末笔均向上、首笔最低末笔最高、价格新高但力度背离）
bool check_first_sell(const std::vector<BI>& bis);

}  // namespace czsc::analyze
