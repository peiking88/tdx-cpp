// RawBar 方法实现 + JSON 序列化
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/bar.rs:34-69 + 296-308
#include "czsc/types/raw_bar.hpp"

#include <nlohmann/json.hpp>

namespace czsc {

bool operator==(const RawBar& a, const RawBar& b) noexcept {
  return a.symbol == b.symbol &&
         a.dt == b.dt &&
         a.freq == b.freq &&
         a.id == b.id &&
         a.open == b.open &&
         a.close == b.close &&
         a.high == b.high &&
         a.low == b.low &&
         a.vol == b.vol &&
         a.amount == b.amount;
}

void to_json(nlohmann::json& j, const RawBar& b) {
  j = nlohmann::json{
    {"symbol", b.symbol},
    {"dt",     b.dt},
    {"freq",   FreqName(b.freq)},
    {"id",     b.id},
    {"open",   b.open},
    {"close",  b.close},
    {"high",   b.high},
    {"low",    b.low},
    {"vol",    b.vol},
    {"amount", b.amount}
  };
}

void from_json(const nlohmann::json& j, RawBar& b) {
  j.at("symbol").get_to(b.symbol);
  j.at("dt").get_to(b.dt);
  if (j.at("freq").is_string()) {
    b.freq = FreqFromString(j.at("freq").get<std::string>());
  }
  j.at("id").get_to(b.id);
  j.at("open").get_to(b.open);
  j.at("close").get_to(b.close);
  j.at("high").get_to(b.high);
  j.at("low").get_to(b.low);
  j.at("vol").get_to(b.vol);
  j.at("amount").get_to(b.amount);
}

}  // namespace czsc
