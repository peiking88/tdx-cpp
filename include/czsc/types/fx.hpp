// FX：分型（顶分型/底分型）
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/fx.rs:35-103
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "czsc/types/enums.hpp"
#include "czsc/types/new_bar.hpp"

namespace czsc {

struct FX {
  std::string symbol;
  int64_t dt = 0;
  Mark mark = Mark::kD;
  double high = 0.0;
  double low = 0.0;
  double fx = 0.0;                    // 分型极值价格
  std::vector<NewBar> elements;       // 构成分型的三根无包含K线（fx.rs:45）

  // 分型力度："强"/"中"/"弱"（fx.rs:53-79 _power_str）
  const char* power_str() const;
  // 成交量力度：三根K线 vol 之和（fx.rs:82-85 _power_volume）
  double power_volume() const noexcept;
  // 是否有重叠中枢（fx.rs:87-103 _has_zs）
  bool has_zs() const noexcept;
};

bool operator==(const FX& a, const FX& b) noexcept;
inline bool operator!=(const FX& a, const FX& b) noexcept { return !(a == b); }

void to_json(nlohmann::json& j, const FX& fx);
void from_json(const nlohmann::json& j, FX& fx);

}  // namespace czsc
