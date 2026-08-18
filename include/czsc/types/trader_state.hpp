// TraderState：交易员多频状态接口（对齐 Rust state.rs TraderState trait）
//
// 将多频 CZSC 访问抽象为接口，使 trader-level 信号函数（cxt_intraday、
// cxt_zhong_shu_gong_zhen 及未来的 pos_*）可按频率名获取对应 CZSC，
// 而无需直接依赖具体的 Trader 实现。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "czsc/analyze/czsc.hpp"
#include "czsc/types/raw_bar.hpp"

namespace czsc {
namespace analyze { class CZSC; }  // 前向声明（定义在 czsc/analyze/czsc.hpp）

// 抽象交易员状态接口（对应 Rust trait TraderState）
class TraderState {
 public:
  virtual ~TraderState() = default;
  // 按频率名查询 CZSC 解析器（如 "日线"/"60分钟"/"30分钟"）
  virtual const analyze::CZSC* get_czsc(const std::string& freq) const = 0;
};

// 简单实现：持有 freq → CZSC 的映射（用于测试与轻量场景）
class SimpleTraderState : public TraderState {
 public:
  SimpleTraderState() = default;

  // 注册某频率的 CZSC（允许后续更新）
  void set_czsc(const std::string& freq, analyze::CZSC czsc) {
    czscs_[freq] = std::move(czsc);
  }

  // 便捷构造：从 (freq, bars) 批量构建
  void add_freq(const std::string& freq, std::vector<RawBar> bars,
                size_t max_bi = 50, size_t min_bi = 6) {
    czscs_.emplace(freq, analyze::CZSC(std::move(bars), max_bi, min_bi));
  }

  const analyze::CZSC* get_czsc(const std::string& freq) const override {
    auto it = czscs_.find(freq);
    return it != czscs_.end() ? &it->second : nullptr;
  }

 private:
  std::unordered_map<std::string, analyze::CZSC> czscs_;
};

}  // namespace czsc
