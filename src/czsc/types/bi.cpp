// BI 笔方法实现 + JSON 序列化
// 直译自 ~/peiking88/czsc/crates/czsc-core/src/objects/bi.rs:308-552
#include "czsc/types/bi.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

namespace czsc {

// 辅助：四舍五入到 n 位小数
namespace {
inline double round_n(double v, int n) {
  double scale = std::pow(10.0, n);
  return std::round(v * scale) / scale;
}
}  // namespace

// 价差力度（bi.rs:332-334）
double BI::get_power_price() const noexcept {
  return round_n(std::abs(fx_b.fx - fx_a.fx), 2);
}

// 成交量力度（bi.rs:342-351）
double BI::get_power_volume() const noexcept {
  if (bars.size() <= 2) return 0.0;
  double total = 0.0;
  // bars[1..bars.size()-1)
  for (size_t i = 1; i + 1 < bars.size(); ++i) {
    total += bars[i].vol;
  }
  return total;
}

// SNR力度（bi.rs:355-358）
double BI::get_power_snr() const noexcept {
  return round_n(get_snr(), 4);
}

// 涨跌幅（bi.rs:361-368）
double BI::get_change() const noexcept {
  if (fx_a.fx == 0.0) return 0.0;
  return round_n((fx_b.fx - fx_a.fx) / fx_a.fx, 4);
}

// 笔内部信噪比（bi.rs:371-396）
double BI::get_snr() const noexcept {
  auto raw_bars = get_raw_bars();
  size_t n = raw_bars.size();

  if (n == 0) return 0.0;
  if (n == 1) return std::abs(raw_bars[0].close - raw_bars[0].open);

  double total_change = std::abs(raw_bars[n - 1].close - raw_bars[0].open);
  double diff_abs = 0.0;
  for (const auto& bar : raw_bars) {
    diff_abs += std::abs(bar.close - bar.open);
  }
  if (diff_abs == 0.0) return 0.0;
  return total_change / diff_abs;
}

// 笔内部高低点斜率（bi.rs:399-432）
double BI::get_slope() const noexcept {
  auto raw_bars = get_raw_bars();
  if (raw_bars.size() < 2) return 0.0;

  double n = static_cast<double>(raw_bars.size());
  double x_mean = (n - 1.0) / 2.0;

  double y_sum = 0.0;
  for (const auto& bar : raw_bars) y_sum += bar.close;
  double y_mean = y_sum / n;

  double numerator = 0.0;
  double denominator = 0.0;
  for (size_t i = 0; i < raw_bars.size(); ++i) {
    double x = static_cast<double>(i);
    double dx = x - x_mean;
    double dy = raw_bars[i].close - y_mean;
    numerator += dx * dy;
    denominator += dx * dx;
  }

  if (denominator == 0.0) return 0.0;
  return numerator / denominator;
}

// 二次项拟合系数（克莱默法则，bi.rs:453-502）
double BI::quadratic_fit_a(const std::vector<double>& closes) const noexcept {
  size_t n_val = closes.size();
  double n = static_cast<double>(n_val);

  double sum_x4 = 0.0, sum_x3 = 0.0, sum_x2 = 0.0, sum_x = 0.0;
  double sum_x2_y = 0.0, sum_x_y = 0.0, sum_y = 0.0;

  for (size_t i = 0; i < n_val; ++i) {
    double x = static_cast<double>(i);
    double x2 = x * x;
    double x3 = x2 * x;
    double x4 = x3 * x;
    double y = closes[i];

    sum_x4 += x4;
    sum_x3 += x3;
    sum_x2 += x2;
    sum_x += x;
    sum_x2_y += x2 * y;
    sum_x_y += x * y;
    sum_y += y;
  }

  // det = sum_x4 * (sum_x2 * n - sum_x * sum_x)
  //     - sum_x3 * (sum_x3 * n - sum_x * sum_x2)
  //     + sum_x2 * (sum_x3 * sum_x - sum_x2 * sum_x2)
  double det = sum_x4 * (sum_x2 * n - sum_x * sum_x)
             - sum_x3 * (sum_x3 * n - sum_x * sum_x2)
             + sum_x2 * (sum_x3 * sum_x - sum_x2 * sum_x2);

  if (std::abs(det) < 1e-10) return 0.0;

  double det_a = sum_x2_y * (sum_x2 * n - sum_x * sum_x)
               - sum_x_y * (sum_x3 * n - sum_x * sum_x2)
               + sum_y * (sum_x3 * sum_x - sum_x2 * sum_x2);

  return det_a / det;
}

// 加速度（bi.rs:437-448）
double BI::get_acceleration() const noexcept {
  auto raw_bars = get_raw_bars();
  if (raw_bars.size() < 3) return 0.0;

  std::vector<double> closes;
  closes.reserve(raw_bars.size());
  for (const auto& bar : raw_bars) closes.push_back(bar.close);

  return quadratic_fit_a(closes);
}

// 斜边长度（bi.rs:542-544）
double BI::get_hypotenuse() const noexcept {
  double raw_n = static_cast<double>(get_raw_bars().size());
  return std::sqrt(get_power_price() * get_power_price() + raw_n * raw_n);
}

// 夹角（度）（bi.rs:547-551）
double BI::get_angle() const noexcept {
  double hyp = get_hypotenuse();
  if (hyp == 0.0) return 0.0;
  double ratio = get_power_price() / hyp;
  if (ratio > 1.0) ratio = 1.0;
  if (ratio < -1.0) ratio = -1.0;
  double angle_rad = std::asin(ratio);
  double angle_deg = angle_rad * 180.0 / M_PI;
  return round_n(angle_deg, 2);
}

// R² 拟合优度（bi.rs:510-521）— 线性回归决定系数，对齐 Rust single_linear()
double BI::get_rsq() const noexcept {
  auto raw_bars = get_raw_bars();
  size_t n = raw_bars.size();
  if (n == 0) return 0.0;

  double sample_size = static_cast<double>(n);

  // x = [0, 1, 2, ..., n-1]，用等差数列公式
  double sum_x = (sample_size - 1.0) * sample_size / 2.0;
  double sum_x_squared = (sample_size - 1.0) * sample_size * (2.0 * sample_size - 1.0) / 6.0;

  double sum_xy = 0.0, sum_y = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double y = raw_bars[i].close;
    sum_xy += static_cast<double>(i) * y;
    sum_y += y;
  }

  double denominator = sample_size * sum_x_squared - sum_x * sum_x;
  if (denominator == 0.0) return 0.0;

  double y_intercept = (sum_x_squared * sum_y - sum_x * sum_xy) / denominator;
  double slope = (sample_size * sum_xy - sum_x * sum_y) / denominator;

  double y_mean = sum_y / sample_size;
  double ss_tot = 0.0, ss_err = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double y = raw_bars[i].close;
    double y_diff = y - y_mean;
    double predicted = slope * static_cast<double>(i) + y_intercept;
    double err = y - predicted;
    ss_tot += y_diff * y_diff;
    ss_err += err * err;
  }

  // 对齐 Rust: rsq = 1.0 - ss_err / (ss_tot + 0.00001)
  double rsq = 1.0 - ss_err / (ss_tot + 0.00001);
  return round_n(rsq, 4);
}

// 原始K线（bi.rs:524-539）
std::vector<RawBar> BI::get_raw_bars() const {
  if (bars.size() <= 2) return {};

  size_t capacity = 0;
  for (size_t i = 1; i + 1 < bars.size(); ++i) {
    capacity += bars[i].elements.size();
  }

  std::vector<RawBar> result;
  result.reserve(capacity);
  for (size_t i = 1; i + 1 < bars.size(); ++i) {
    result.insert(result.end(), bars[i].elements.begin(), bars[i].elements.end());
  }
  return result;
}

bool operator==(const BI& a, const BI& b) noexcept {
  return a.symbol == b.symbol &&
         a.fx_a == b.fx_a &&
         a.fx_b == b.fx_b &&
         a.fxs == b.fxs &&
         a.direction == b.direction &&
         a.bars == b.bars;
}

void to_json(nlohmann::json& j, const BI& bi) {
  j = nlohmann::json{
    {"symbol",    bi.symbol},
    {"fx_a",      bi.fx_a},
    {"fx_b",      bi.fx_b},
    {"fxs",       bi.fxs},
    {"direction", DirectionName(bi.direction)},
    {"bars",      bi.bars}
  };
}

void from_json(const nlohmann::json& j, BI& bi) {
  j.at("symbol").get_to(bi.symbol);
  j.at("fx_a").get_to(bi.fx_a);
  j.at("fx_b").get_to(bi.fx_b);
  j.at("fxs").get_to(bi.fxs);
  if (j.at("direction").is_string()) {
    std::string d = j.at("direction").get<std::string>();
    if (d == "Down") bi.direction = Direction::kDown;
    else             bi.direction = Direction::kUp;
  }
  j.at("bars").get_to(bi.bars);
}

}  // namespace czsc
