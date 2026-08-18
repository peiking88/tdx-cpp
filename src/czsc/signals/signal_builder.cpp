// 信号构造工具实现
#include "czsc/signals/signal_builder.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace czsc::signals {

std::vector<Signal> make_signal7(const char* k1, const char* k2, const char* k3,
                                 const char* v1, const char* v2, const char* v3, int score) {
  std::ostringstream oss;
  oss << k1 << "_" << k2 << "_" << k3 << "_" << v1 << "_" << v2 << "_" << v3 << "_" << score;
  return {Signal::FromString(oss.str())};
}

// pd_cut_last_label 分箱
size_t pd_cut_last_label(const std::vector<double>& values, size_t n) {
  if (n == 0 || values.empty()) return 0;
  for (auto v : values) if (!std::isfinite(v)) return 0;

  double min_v = *std::min_element(values.begin(), values.end());
  double max_v = *std::max_element(values.begin(), values.end());
  if (!std::isfinite(min_v) || !std::isfinite(max_v)) return 0;

  double x = values.back();

  // 常量序列：左右各 0.1%
  std::vector<double> bins(n + 1);
  if (std::abs(max_v - min_v) < 1e-12) {
    double delta = (min_v != 0.0) ? 0.001 * std::abs(min_v) : 0.001;
    for (size_t i = 0; i <= n; ++i)
      bins[i] = (min_v - delta) + ((max_v + delta) - (min_v - delta)) * i / (double)n;
  } else {
    for (size_t i = 0; i <= n; ++i)
      bins[i] = min_v + (max_v - min_v) * i / (double)n;
    bins[0] -= (max_v - min_v) * 0.001;  // 只扩展左边界
  }

  // lower_bound: 第一个 >= x
  auto it = std::lower_bound(bins.begin(), bins.end(), x);
  size_t idx = it - bins.begin();
  if (idx == 0) return 1;
  if (idx >= bins.size()) return n;
  return idx;
}

// 等宽分箱
size_t cut_last_bin_label(const std::vector<double>& values, size_t n) {
  if (n == 0 || values.empty()) return 0;

  double min_v = INFINITY, max_v = -INFINITY;
  bool has_finite = false;
  for (auto v : values) {
    if (std::isfinite(v)) {
      has_finite = true;
      if (v < min_v) min_v = v;
      if (v > max_v) max_v = v;
    }
  }
  if (!has_finite || !std::isfinite(min_v) || !std::isfinite(max_v)) return 0;
  if (std::abs(max_v - min_v) < 1e-12) return (n + 1) / 2;

  double last = values.back();
  if (!std::isfinite(last)) return 0;

  double width = (max_v - min_v) / (double)n;
  if (width <= 0.0) return 1;

  int idx = (int)std::floor((last - min_v) / width) + 1;
  if (idx < 1) idx = 1;
  if (idx > (int)n) idx = (int)n;
  return (size_t)idx;
}

// 分位数分箱
int qcut_last_label(const std::vector<double>& values, size_t q) {
  if (q == 0 || values.empty()) return -1;

  std::vector<double> sorted;
  for (auto v : values) if (std::isfinite(v)) sorted.push_back(v);
  if (sorted.empty()) return -1;
  std::sort(sorted.begin(), sorted.end());

  double x = values.back();
  if (!std::isfinite(x)) return -1;

  auto quant = [&](double p) -> double {
    if (sorted.size() == 1) return sorted[0];
    double h = (sorted.size() - 1) * p;
    size_t i = (size_t)h;
    size_t j = (size_t)std::ceil(h);
    if (i == j) return sorted[i];
    return sorted[i] + (h - i) * (sorted[j] - sorted[i]);
  };

  std::vector<double> edges;
  for (size_t i = 0; i <= q; ++i) edges.push_back(quant((double)i / q));
  // dedup
  auto last = std::unique(edges.begin(), edges.end(),
                          [](double a, double b) { return std::abs(a - b) < 1e-12; });
  edges.erase(last, edges.end());
  if (edges.size() <= 1) return -1;

  size_t nbins = edges.size() - 1;
  if (x < edges[0] || x > edges[nbins]) return -1;
  for (size_t i = 0; i < nbins; ++i) {
    bool ok = (i == 0) ? (x >= edges[i]) : (x > edges[i]);
    if (ok && x <= edges[i + 1]) return (int)i;
  }
  return -1;
}

// 快慢线交叉
std::vector<CrossEntry> fast_slow_cross(const std::vector<double>& fast,
                                        const std::vector<double>& slow) {
  std::vector<CrossEntry> res;
  size_t len = std::min(fast.size(), slow.size());
  if (len < 2) return res;

  size_t last_idx = 0;
  for (size_t i = 2; i < len; ++i) {
    double f0 = fast[i - 1], s0 = slow[i - 1];
    double f1 = fast[i], s1 = slow[i];
    if (f0 <= s0 && f1 > s1) {
      res.push_back({1.0, f1, s1, (double)(i - last_idx)});
      last_idx = i;
    } else if (f0 >= s0 && f1 < s1) {
      res.push_back({-1.0, f1, s1, (double)(i - last_idx)});
      last_idx = i;
    }
  }
  return res;
}

std::pair<size_t, size_t> cal_cross_num(const std::vector<CrossEntry>& cross, size_t distance) {
  if (cross.empty()) return {0, 0};

  auto filtered = cross;
  if (filtered.size() > 1) {
    filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
                   [&](const CrossEntry& e) { return e.distance < (double)distance; }),
                   filtered.end());
  }

  size_t jc = 0, sc = 0;
  for (auto& e : filtered) {
    if (e.type > 0) ++jc;
    else if (e.type < 0) ++sc;
  }
  return {jc, sc};
}

double linear_slope(const std::vector<double>& y) {
  size_t n = y.size();
  if (n < 2) return 0.0;

  double nf = (double)n;
  double sum_x = (nf - 1.0) * nf / 2.0;
  double sum_xx = (nf - 1.0) * nf * (2.0 * nf - 1.0) / 6.0;
  double sum_y = 0.0, sum_xy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sum_y += y[i];
    sum_xy += (double)i * y[i];
  }
  double denom = nf * sum_xx - sum_x * sum_x;
  if (std::abs(denom) < 1e-12) return 0.0;
  return (nf * sum_xy - sum_x * sum_y) / denom;
}

size_t down_cross_count(const std::vector<double>& x1, const std::vector<double>& x2) {
  if (x1.size() != x2.size() || x1.size() < 2) return 0;
  size_t num = 0;
  for (size_t i = 0; i + 1 < x1.size(); ++i) {
    bool b1 = x1[i] < x2[i];
    bool b2 = x1[i + 1] < x2[i + 1];
    if (b2 && b1 != b2) ++num;
  }
  return num;
}

}  // namespace czsc::signals
