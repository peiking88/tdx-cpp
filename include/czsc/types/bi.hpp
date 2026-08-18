// BI：笔
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/bi.rs:28-47 + 308-552
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "czsc/types/enums.hpp"
#include "czsc/types/fx.hpp"

namespace czsc {

struct BI {
  std::string symbol;
  FX fx_a;                    // 笔开始的分型（bi.rs:36）
  FX fx_b;                    // 笔结束的分型（bi.rs:38）
  std::vector<FX> fxs;       // 笔内部的分型列表（bi.rs:40）
  Direction direction = Direction::kUp;
  std::vector<NewBar> bars;  // 构成笔的无包含K线序列（bi.rs:42）

  // ---- 17 个力度/属性方法（bi.rs:308-552）----

  int64_t start_dt() const noexcept { return fx_a.dt; }
  int64_t end_dt() const noexcept   { return fx_b.dt; }

  double get_high() const noexcept { return std::fmax(fx_a.high, fx_b.high); }
  double get_low() const noexcept  { return std::fmin(fx_a.low, fx_b.low); }

  // 价差力度：|fx_b.fx - fx_a.fx| 保留2位小数（bi.rs:332-334）
  double get_power_price() const noexcept;
  double get_power() const noexcept { return get_power_price(); }

  // 成交量力度：bars[1:-1].vol sum（bi.rs:342-351）
  double get_power_volume() const noexcept;

  // SNR力度：round(snr * 10000) / 10000（bi.rs:355-358）
  double get_power_snr() const noexcept;

  // 涨跌幅：（bi.rs:361-368）
  double get_change() const noexcept;

  // 笔内部信噪比（bi.rs:371-396）
  double get_snr() const noexcept;

  // 笔内部高低点斜率（bi.rs:399-432）
  double get_slope() const noexcept;

  // 笔内部价格加速度（bi.rs:437-448）
  double get_acceleration() const noexcept;

  // 无包含K线数量（bi.rs:505-507）
  size_t get_length() const noexcept { return bars.size(); }

  // R² 拟合优度（bi.rs:510-521）— 线性回归决定系数
  double get_rsq() const noexcept;

  // 斜边长度（bi.rs:542-544）
  double get_hypotenuse() const noexcept;

  // 夹角（度）（bi.rs:547-551）
  double get_angle() const noexcept;

  // 原始K线序列（不含首尾分型的首根K线）（bi.rs:524-539）
  std::vector<RawBar> get_raw_bars() const;

  // 次项系数多项式拟合（bi.rs:453-502 numpy_compatible_quadratic_fit）
  // ponytail: 克莱默法则 3×3 直接解，与 numpy.polyfit(degree=2) 数学等价
  double quadratic_fit_a(const std::vector<double>& closes) const noexcept;
};

bool operator==(const BI& a, const BI& b) noexcept;
inline bool operator!=(const BI& a, const BI& b) noexcept { return !(a == b); }

void to_json(nlohmann::json& j, const BI& bi);
void from_json(const nlohmann::json& j, BI& bi);

}  // namespace czsc
