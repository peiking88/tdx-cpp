// ZS 中枢方法实现 + JSON 序列化
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/zs.rs:47-100
#include "czsc/types/zs.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

namespace czsc {

// 从笔列表构造中枢（zs.rs:47-87）
ZS::ZS(const std::vector<BI>& bis) : bis(bis) {
  if (bis.empty()) return;

  sdt = bis.front().start_dt();
  edt = bis.back().end_dt();
  sdir = bis.front().direction;
  edir = bis.back().direction;

  // zg = min(highs[0..3])（zs.rs:52-56）
  zg = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < std::min(size_t(3), bis.size()); ++i) {
    double h = bis[i].get_high();
    if (h < zg) zg = h;
  }

  // zd = max(lows[0..3])（zs.rs:57-61）
  zd = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < std::min(size_t(3), bis.size()); ++i) {
    double l = bis[i].get_low();
    if (l > zd) zd = l;
  }

  // gg = max(bi.high) across ALL（zs.rs:62-65）
  gg = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) {
    double h = bi.get_high();
    if (h > gg) gg = h;
  }

  // dd = min(bi.low) across ALL（zs.rs:66-69）
  dd = std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) {
    double l = bi.get_low();
    if (l < dd) dd = l;
  }

  zz = zd + (zg - zd) * 0.5;  // zs.rs:71
}

// 中枢是否有效（zs.rs:90-100）
bool ZS::is_valid() const noexcept {
  if (zg < zd) return false;
  if (bis.empty()) return false;

  for (const auto& bi : bis) {
    double bh = bi.get_high();
    double bl = bi.get_low();
    bool high_in = (bh <= zg && bh >= zd);
    bool low_in  = (bl <= zg && bl >= zd);
    bool crosses  = (bh >= zg && bl <= zd);
    if (!high_in && !low_in && !crosses) return false;
  }
  return true;
}

bool operator==(const ZS& a, const ZS& b) noexcept {
  return a.bis == b.bis &&
         a.sdt == b.sdt &&
         a.edt == b.edt &&
         a.sdir == b.sdir &&
         a.edir == b.edir &&
         a.zg == b.zg &&
         a.zd == b.zd &&
         a.zz == b.zz &&
         a.gg == b.gg &&
         a.dd == b.dd;
}

void to_json(nlohmann::json& j, const ZS& zs) {
  j = nlohmann::json{
    {"bis",  zs.bis},
    {"sdt",  zs.sdt},
    {"edt",  zs.edt},
    {"sdir", DirectionName(zs.sdir)},
    {"edir", DirectionName(zs.edir)},
    {"zg",   zs.zg},
    {"zd",   zs.zd},
    {"zz",   zs.zz},
    {"gg",   zs.gg},
    {"dd",   zs.dd}
  };
}

void from_json(const nlohmann::json& j, ZS& zs) {
  j.at("bis").get_to(zs.bis);
  j.at("sdt").get_to(zs.sdt);
  j.at("edt").get_to(zs.edt);
  if (j.at("sdir").is_string()) {
    std::string d = j.at("sdir").get<std::string>();
    zs.sdir = (d == "Down") ? Direction::kDown : Direction::kUp;
  }
  if (j.at("edir").is_string()) {
    std::string d = j.at("edir").get<std::string>();
    zs.edir = (d == "Down") ? Direction::kDown : Direction::kUp;
  }
  j.at("zg").get_to(zs.zg);
  j.at("zd").get_to(zs.zd);
  j.at("zz").get_to(zs.zz);
  j.at("gg").get_to(zs.gg);
  j.at("dd").get_to(zs.dd);
}

}  // namespace czsc
