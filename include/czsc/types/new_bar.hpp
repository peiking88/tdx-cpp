// NewBar：去除包含关系后的K线元素
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/bar.rs:311-572
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "czsc/types/enums.hpp"
#include "czsc/types/raw_bar.hpp"

namespace czsc {

struct NewBar {
  std::string symbol;
  int64_t dt = 0;
  Freq freq = Freq::kDay;
  int32_t id = 0;
  double open = 0.0;
  double close = 0.0;
  double high = 0.0;
  double low = 0.0;
  double vol = 0.0;
  double amount = 0.0;
  std::vector<RawBar> elements;  // 被包含关系合并的原始K线（bar.rs:330）

  // 从单根 RawBar 创建（bar.rs:535-572 new_from_raw）
  static NewBar FromRaw(const RawBar& bar);
};

bool operator==(const NewBar& a, const NewBar& b) noexcept;
inline bool operator!=(const NewBar& a, const NewBar& b) noexcept { return !(a == b); }

void to_json(nlohmann::json& j, const NewBar& b);
void from_json(const nlohmann::json& j, NewBar& b);

}  // namespace czsc
