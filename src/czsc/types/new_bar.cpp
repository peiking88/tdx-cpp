// NewBar 方法实现 + JSON 序列化
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/bar.rs:311-572
#include "czsc/types/new_bar.hpp"

#include <nlohmann/json.hpp>

namespace czsc {

NewBar NewBar::FromRaw(const RawBar& bar) {
  NewBar nb;
  nb.symbol = bar.symbol;
  nb.dt = bar.dt;
  nb.freq = bar.freq;
  nb.id = bar.id;
  nb.open = bar.open;
  nb.close = bar.close;
  nb.high = bar.high;
  nb.low = bar.low;
  nb.vol = bar.vol;
  nb.amount = bar.amount;
  nb.elements.push_back(bar);  // bar.rs:549
  return nb;
}

bool operator==(const NewBar& a, const NewBar& b) noexcept {
  return a.symbol == b.symbol &&
         a.dt == b.dt &&
         a.freq == b.freq &&
         a.id == b.id &&
         a.open == b.open &&
         a.close == b.close &&
         a.high == b.high &&
         a.low == b.low &&
         a.vol == b.vol &&
         a.amount == b.amount &&
         a.elements == b.elements;
}

void to_json(nlohmann::json& j, const NewBar& b) {
  j = nlohmann::json{
    {"symbol",   b.symbol},
    {"dt",       b.dt},
    {"freq",     FreqName(b.freq)},
    {"id",       b.id},
    {"open",     b.open},
    {"close",    b.close},
    {"high",     b.high},
    {"low",      b.low},
    {"vol",      b.vol},
    {"amount",   b.amount},
    {"elements", b.elements}
  };
}

void from_json(const nlohmann::json& j, NewBar& b) {
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
  if (j.contains("elements") && j.at("elements").is_array()) {
    j.at("elements").get_to(b.elements);
  }
}

}  // namespace czsc
