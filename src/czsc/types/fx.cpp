// FX 分型方法实现 + JSON 序列化
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/fx.rs:52-103
#include "czsc/types/fx.hpp"

#include <cassert>
#include <cmath>
#include <nlohmann/json.hpp>

namespace czsc {

// power_str 直译 fx.rs:53-79
const char* FX::power_str() const {
  assert(elements.size() == 3);

  const auto& k1 = elements[0];
  const auto& k2 = elements[1];
  const auto& k3 = elements[2];

  switch (mark) {
    case Mark::kD:
      if (k3.close > k1.high)       return "\xe5\xbc\xba";   // 强
      else if (k3.close > k2.high)  return "\xe4\xb8\xad";   // 中
      else                          return "\xe5\xbc\xb1";   // 弱
    case Mark::kG:
      if (k3.close < k1.low)        return "\xe5\xbc\xba";   // 强
      else if (k3.close < k2.low)   return "\xe4\xb8\xad";   // 中
      else                          return "\xe5\xbc\xb1";   // 弱
  }
  return "\xe5\xbc\xb1";  // 弱
}

// fx.rs:82-85
double FX::power_volume() const noexcept {
  double total = 0.0;
  for (const auto& e : elements) total += e.vol;
  return total;
}

// fx.rs:87-103
bool FX::has_zs() const noexcept {
  assert(elements.size() == 3);

  double zd = -std::numeric_limits<double>::infinity();
  double zg = std::numeric_limits<double>::infinity();

  for (const auto& e : elements) {
    if (e.low > zd)  zd = e.low;
    if (e.high < zg) zg = e.high;
  }

  return zg >= zd;
}

bool operator==(const FX& a, const FX& b) noexcept {
  return a.symbol == b.symbol &&
         a.dt == b.dt &&
         a.mark == b.mark &&
         a.high == b.high &&
         a.low == b.low &&
         a.fx == b.fx &&
         a.elements == b.elements;
}

void to_json(nlohmann::json& j, const FX& fx) {
  j = nlohmann::json{
    {"symbol",   fx.symbol},
    {"dt",       fx.dt},
    {"mark",     MarkName(fx.mark)},
    {"high",     fx.high},
    {"low",      fx.low},
    {"fx",       fx.fx},
    {"elements", fx.elements}
  };
}

void from_json(const nlohmann::json& j, FX& fx) {
  j.at("symbol").get_to(fx.symbol);
  j.at("dt").get_to(fx.dt);
  if (j.at("mark").is_string()) {
    std::string m = j.at("mark").get<std::string>();
    if (m == "D") fx.mark = Mark::kD;
    else          fx.mark = Mark::kG;
  }
  j.at("high").get_to(fx.high);
  j.at("low").get_to(fx.low);
  j.at("fx").get_to(fx.fx);
  if (j.contains("elements") && j.at("elements").is_array()) {
    j.at("elements").get_to(fx.elements);
  }
}

}  // namespace czsc
