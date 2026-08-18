// RawBar：原始K线元素
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/bar.rs:34-69
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "czsc/types/enums.hpp"

namespace czsc {

struct RawBar {
  std::string symbol;
  int64_t dt = 0;             // epoch seconds（Rust DateTime<Utc> → C++ int64_t）
  Freq freq = Freq::kDay;
  int32_t id = 0;
  double open = 0.0;
  double close = 0.0;         // 注意：close 在 high 之前（bar.rs:37-38）
  double high = 0.0;
  double low = 0.0;
  double vol = 0.0;
  double amount = 0.0;

  // 上影线（bar.rs:56-58）
  double upper() const noexcept { return high - std::fmax(open, close); }
  // 下影线（bar.rs:61-63）
  double lower() const noexcept { return std::fmin(open, close) - low; }
  // 实体（bar.rs:66-68）
  double solid() const noexcept { return std::abs(open - close); }
};

// 全字段相等（bar.rs:296-308）
bool operator==(const RawBar& a, const RawBar& b) noexcept;
inline bool operator!=(const RawBar& a, const RawBar& b) noexcept { return !(a == b); }

// JSON 序列化（nlohmann ADL）
void to_json(nlohmann::json& j, const RawBar& b);
void from_json(const nlohmann::json& j, RawBar& b);

}  // namespace czsc
