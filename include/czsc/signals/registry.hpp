// 信号注册表：手动 std::unordered_map
// 每个信号函数一条注册行：name → {category, fn_ptr, param_template, opcode}
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "czsc/signals/param_view.hpp"
#include "czsc/types/signal.hpp"
#include "czsc/types/trader_state.hpp"

namespace czsc {

// 前向声明
class RawBar;
namespace ta { struct TaCache; }
namespace analyze { class CZSC; }

namespace signals {

// K线级信号函数签名：CZSC + params + TaCache → Vec<Signal>
using SignalFn = std::function<std::vector<Signal>(
    const analyze::CZSC&, const ParamView&, ta::TaCache*)>;

// 交易员级信号函数签名：TraderState + params → Vec<Signal>
using TraderSignalFn = std::function<std::vector<Signal>(
    const TraderState&, const ParamView&)>;

// 信号类别
enum class SignalCategory { kKline, kTrader };

// 信号元信息
struct SignalMeta {
  SignalCategory category;
  SignalFn func;
  const char* param_template;
  const char* opcode;
};

// 交易员级信号元信息
struct TraderSignalMeta {
  TraderSignalFn func;
  const char* param_template;
  const char* opcode;
};

// 全局信号注册表（懒初始化，首次访问时从宏生成表构建）
const std::unordered_map<std::string, SignalMeta>& signal_registry();

// 交易员级信号注册表
const std::unordered_map<std::string, TraderSignalMeta>& trader_signal_registry();

// 列出所有已注册信号（名称+参数模板+类别+命名空间前缀）
struct RegisteredSignalInfo {
  std::string name;
  std::string param_template;
  std::string category;
  std::string ns;
};
std::vector<RegisteredSignalInfo> list_all_signals();

// 运行单个 K 线级信号
std::vector<Signal> run_signal(const char* name, const analyze::CZSC& czsc,
                               const ParamView& params, ta::TaCache* cache);

// 运行单个交易员级信号
std::vector<Signal> run_trader_signal(const char* name, const TraderState& state,
                                      const ParamView& params);

}  // namespace signals
}  // namespace czsc
