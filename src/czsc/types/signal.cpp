// Signal 信号方法实现 + JSON 序列化
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/signal.rs:83-162
#include "czsc/types/signal.hpp"

#include <nlohmann/json.hpp>
#include <sstream>

namespace czsc {

Signal Signal::FromString(const std::string& s) {
  // 按 '_' 切分 7 部分
  std::vector<std::string> parts;
  size_t pos = 0;
  std::string ss = s;
  while (pos < ss.size()) {
    size_t next = ss.find('_', pos);
    if (next == std::string::npos) {
      parts.push_back(ss.substr(pos));
      break;
    }
    parts.push_back(ss.substr(pos, next - pos));
    pos = next + 1;
  }

  if (parts.size() != 7) {
    throw std::invalid_argument(
        "Signal格式无效：应该为 k1_k2_k3_v1_v2_v3_score 格式 (7个部分)，got=" +
        std::to_string(parts.size()) + " input=" + s);
  }

  int32_t sc = std::stoi(parts[6]);
  if (sc < 0 || sc > 100) {
    throw std::invalid_argument("score 必须在0~100之间，got=" + std::to_string(sc));
  }

  Signal sig;
  sig.k1 = parts[0];
  sig.k2 = parts[1];
  sig.k3 = parts[2];
  sig.v1 = parts[3];
  sig.v2 = parts[4];
  sig.v3 = parts[5];
  sig.score = sc;
  return sig;
}

std::string Signal::to_string() const {
  return k1 + "_" + k2 + "_" + k3 + "_" + v1 + "_" + v2 + "_" + v3 + "_" + std::to_string(score);
}

// 过滤"任意"后的 key（signal.rs:113-125）
std::string Signal::key() const {
  std::vector<std::string> key_parts;
  if (k1 != kSignalAny) key_parts.push_back(k1);
  if (k2 != kSignalAny) key_parts.push_back(k2);
  if (k3 != kSignalAny) key_parts.push_back(k3);
  if (key_parts.empty()) return "";
  std::string result;
  for (size_t i = 0; i < key_parts.size(); ++i) {
    if (i > 0) result += "_";
    result += key_parts[i];
  }
  return result;
}

// value: v1_v2_v3_score（signal.rs:129-131）
std::string Signal::value() const {
  return v1 + "_" + v2 + "_" + v3 + "_" + std::to_string(score);
}

// Python 语义匹配（signal.rs:134-161）
bool Signal::is_match(
    const std::unordered_map<std::string, std::string>& signals_dict) const {
  auto k = key();
  auto it = signals_dict.find(k);
  if (it == signals_dict.end()) return false;

  // 解析字典值: v1_v2_v3_score
  const auto& val = it->second;
  std::vector<std::string> vparts;
  size_t pos = 0;
  while (pos < val.size()) {
    size_t next = val.find('_', pos);
    if (next == std::string::npos) {
      vparts.push_back(val.substr(pos));
      break;
    }
    vparts.push_back(val.substr(pos, next - pos));
    pos = next + 1;
  }
  if (vparts.size() != 4) return false;

  const auto& dv1 = vparts[0];
  const auto& dv2 = vparts[1];
  const auto& dv3 = vparts[2];
  int32_t dscore = 0;
  try {
    dscore = std::stoi(vparts[3]);
  } catch (const std::exception&) {
    return false;  // score 非数字 → 视为不匹配
  }

  if (dscore < score) return false;
  if (v1 != kSignalAny && v1 != dv1) return false;
  if (v2 != kSignalAny && v2 != dv2) return false;
  if (v3 != kSignalAny && v3 != dv3) return false;
  return true;
}

bool operator==(const Signal& a, const Signal& b) noexcept {
  return a.to_string() == b.to_string();
}

void to_json(nlohmann::json& j, const Signal& sig) {
  j = sig.to_string();  // Signal 序列化为完整字符串
}

void from_json(const nlohmann::json& j, Signal& sig) {
  sig = Signal::FromString(j.get<std::string>());
}

}  // namespace czsc
