// Signal：信号类型
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/signal.rs:30-47 + 83-162
// 格式: "k1_k2_k3_v1_v2_v3_score"（7 部分，6 个下划线）
#pragma once

#include <cstdint>
#include <string>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace czsc {

inline constexpr const char* kSignalAny = "\xe4\xbb\xbb\xe6\x84\x8f";  // 任意

struct Signal {
  std::string k1 = kSignalAny;
  std::string k2 = kSignalAny;
  std::string k3 = kSignalAny;
  std::string v1 = kSignalAny;
  std::string v2 = kSignalAny;
  std::string v3 = kSignalAny;
  int32_t score = 0;

  Signal() = default;

  // 从 "k1_k2_k3_v1_v2_v3_score" 字符串解析（signal.rs:83-110）
  static Signal FromString(const std::string& s);

  // 完整信号字符串（signal.rs:168-170 Display）
  std::string to_string() const;

  // 过滤"任意"后的 key（signal.rs:113-125）
  std::string key() const;

  // value: v1_v2_v3_score（signal.rs:129-131）
  std::string value() const;

  // 按 Python 语义匹配（signal.rs:134-161）
  bool is_match(const std::unordered_map<std::string, std::string>& signals_dict) const;
};

bool operator==(const Signal& a, const Signal& b) noexcept;
inline bool operator!=(const Signal& a, const Signal& b) noexcept { return !(a == b); }

// JSON: Signal 序列化为完整字符串 "k1_k2_k3_v1_v2_v3_score"
void to_json(nlohmann::json& j, const Signal& sig);
void from_json(const nlohmann::json& j, Signal& sig);

}  // namespace czsc
