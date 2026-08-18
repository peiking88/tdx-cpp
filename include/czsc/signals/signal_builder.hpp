// 信号构造工具函数
// 直译自 ~/peiking88/czsc/crates/czsc-signals/src/utils/sig.rs
#pragma once

#include <string>
#include <vector>

#include "czsc/types/enums.hpp"
#include "czsc/types/raw_bar.hpp"
#include "czsc/types/signal.hpp"

namespace czsc::signals {

// ---- 核心构建器 ----

// 底层 7 段信号构造
std::vector<Signal> make_signal7(const char* k1, const char* k2, const char* k3,
                                 const char* v1, const char* v2, const char* v3, int score);

// v1+v2 双值信号
inline std::vector<Signal> make_signal(const std::string& k1, const std::string& k2,
                                       const std::string& k3, const std::string& v1,
                                       const std::string& v2) {
  return make_signal7(k1.c_str(), k2.c_str(), k3.c_str(), v1.c_str(), v2.c_str(),
                      kSignalAny, 0);
}

// v1 单值信号（v2/v3="任意", score=0）
// 参数为 const std::string&：消除调用方 ("D"+F(di)).c_str() 的临时量脆弱性；
// const char* 字面量仍兼容（隐式构造临时 string，生命周期覆盖全表达式）。
inline std::vector<Signal> make_signal_v1(const std::string& k1, const std::string& k2,
                                          const std::string& k3, const std::string& v1) {
  return make_signal7(k1.c_str(), k2.c_str(), k3.c_str(), v1.c_str(),
                      kSignalAny, kSignalAny, 0);
}
inline std::vector<Signal> make_kline_signal_v1(const std::string& k1, const std::string& k2,
                                                const std::string& k3, const std::string& v1) {
  return make_signal_v1(k1, k2, k3, v1);
}

// v1+v2 K线信号（v3="任意"）
inline std::vector<Signal> make_kline_signal_v2(const std::string& k1, const std::string& k2,
                                                const std::string& k3, const std::string& v1,
                                                const std::string& v2) {
  return make_signal7(k1.c_str(), k2.c_str(), k3.c_str(), v1.c_str(), v2.c_str(),
                      kSignalAny, 0);
}

// v1+v2+v3 K线全信号
inline std::vector<Signal> make_kline_signal_v3(const std::string& k1, const std::string& k2,
                                                const std::string& k3, const std::string& v1,
                                                const std::string& v2, const std::string& v3) {
  return make_signal7(k1.c_str(), k2.c_str(), k3.c_str(), v1.c_str(), v2.c_str(), v3.c_str(), 0);
}

// ---- 切片与工具 ----

// 取截止到倒数第 di 个元素的前 n 个元素（对齐 Python get_sub_elements）
template <typename T>
inline std::vector<T> get_sub_elements_vec(const std::vector<T>& els, size_t di, size_t n) {
  if (els.empty() || di < 1 || di > els.size()) return {};
  if (n == 0) {
    if (di == 1) return els;
    return {};
  }
  size_t end = els.size() - di + 1;
  size_t start = (end > n) ? (end - n) : 0;
  return {els.begin() + start, els.begin() + end};
}

// 统计末尾连续相同元素个数
template <typename T>
inline size_t count_last_same(const std::vector<T>& seq) {
  if (seq.empty()) return 0;
  const auto& last = seq.back();
  size_t c = 0;
  for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
    if (*it == last) ++c; else break;
  }
  return c;
}

// pd.cut 分箱——返回最后一个值的分箱标签（qcut_last_label）
size_t pd_cut_last_label(const std::vector<double>& values, size_t n);

// 等宽分箱
size_t cut_last_bin_label(const std::vector<double>& values, size_t n);

// 分位数分箱
int qcut_last_label(const std::vector<double>& values, size_t q);

// 快慢线交叉
struct CrossEntry { double type, fast, slow, distance; };
std::vector<CrossEntry> fast_slow_cross(const std::vector<double>& fast,
                                        const std::vector<double>& slow);

// 计算金叉/死叉次数
std::pair<size_t, size_t> cal_cross_num(const std::vector<CrossEntry>& cross, size_t distance);

// 线性回归斜率
double linear_slope(const std::vector<double>& y);

// 向下穿越计数
size_t down_cross_count(const std::vector<double>& x1, const std::vector<double>& x2);

}  // namespace czsc::signals
