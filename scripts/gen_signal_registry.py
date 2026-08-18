#!/usr/bin/env python3
"""从 Rust #[signal] 属性生成完整 C++ registry.cpp"""
import re, os
from pathlib import Path

BASE = Path.home() / "peiking88/czsc/crates/czsc-signals/src/"
OUT = Path.home() / "peiking88/czsc-cpp/src/signals/registry.cpp"

# 提取 Rust signal 中的中文字符串映射
ZH_MAP = {
    "单K趋势": "单K趋势", "分类": "分类", "涨跌停": "涨跌停",
    "交叉": "交叉", "无": "无", "向上": "向上", "向下": "向下",
    "其他": "其他", "放量": "放量", "缩量": "缩量",
    "一买": "一买", "二买": "二买", "三买": "三买",
    "一卖": "一卖", "二卖": "二卖", "三卖": "三卖",
    "第{}层": "第{}层", "笔状态": "笔状态",
    "单量": "单量", "涨停": "涨停", "跌停": "跌停",
    "BS辅助": "BS辅助",
}

# 收集所有信号
signals = []
for fname in sorted(os.listdir(BASE)):
    if not fname.endswith('.rs'): continue
    text = Path(BASE / fname).read_text()
    for m in re.finditer(
        r'#\[signal\(\s*category\s*=\s*"(\w+)"\s*,\s*name\s*=\s*"([^"]+)"\s*,\s*template\s*=\s*"([^"]+)"\s*,\s*opcode\s*=\s*"([^"]+)"\s*,\s*param_kind\s*=\s*"([^"]+)"',
        text):
        sig = dict(category=m.group(1), name=m.group(2), template=m.group(3),
                   opcode=m.group(4), param_kind=m.group(5), file=fname.replace('.rs',''))
        signals.append(sig)

# 按模块分组
by_file = {}
for s in signals:
    by_file.setdefault(s['file'], []).append(s)

# 实现模式：根据模块/param_kind 选择 stubs
# bar → 简单的 K 线形态判断
# tas → TA 指标信号 (需要 TaCache)
# cxt → 缠论上下文 (需要 CZSC.bi_list)
# pos → 仓位管理 (trader 类型)
# ang/xl/vol/pressure/byi/coo/jcc/zdy → 各自的简单模式
# clv/cvolp/kcatr/ntmdk/obv → 单行线信号
# cat/cxt_trader/zdy_trader → trader 复合信号

HEADER = '''// 信号注册表 + 信号函数实现（自动生成 + 手动实现）
// 从 Rust #[signal] 属性生成，{} signals total
// Source: ~/peiking88/czsc/crates/czsc-signals/src/

#include "czsc/signals/registry.hpp"
#include "czsc/signals/signal_builder.hpp"
#include "czsc/ta/indicators.hpp"
#include "czsc/ta/ta_cache.hpp"
#include "czsc/analyze/czsc.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace czsc::signals {{

using czsc::analyze::CZSC;
using czsc::ta::TaCache;
using czsc::ta::ma_cache_key;
using czsc::ta::macd_cache_key;
using czsc::ta::boll_cache_key;
using czsc::ta::kdj_cache_key;
using czsc::ta::update_ma_cache;
using czsc::ta::update_vol_ma_cache;
using czsc::ta::update_macd_cache;
using czsc::ta::update_boll_cache;
using czsc::ta::update_kdj_cache;
using czsc::ta::update_rsi_cache;
using czsc::ta::update_atr_cache;
using czsc::ta::update_cci_cache;
using czsc::ta::update_sar_cache;

// ============================================================
// Helper: 数值 → 字符串（用于拼接缓存 key / D1/D2 等）
// ============================================================
static std::string F(size_t n) {{ return std::to_string(n); }}

'''.format(len(signals))

# 生成 bar 信号 stubs (46 个)
# 注：make_kline_signal_v1 参数为 const std::string&，直接传字符串 / 临时量即可，无需 .c_str()
BAR_BODIES = '''
// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> {fn}(const CZSC& c, const ParamView& p, TaCache*) {{
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "{opcode}", "\xe5\x85\xb6\xe4\xbb\x96");
}}
'''

# 生成 tas 信号 stubs (59个)
TAS_BODIES = '''
// tas: TA指标信号 ({count} 个)
static std::vector<Signal> {fn}(const CZSC& c, const ParamView& p, TaCache* cache) {{
  if (!cache) return {{}};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {{}};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "{opcode}", "\xe5\x85\xb6\xe4\xbb\x96");
}}
'''

# 生成 cxt 信号 stubs (41个)
CXT_BODIES = '''
// cxt: 缠论上下文 ({count} 个)
static std::vector<Signal> {fn}(const CZSC& c, const ParamView& p, TaCache*) {{
  std::string v1 = "\xe6\x97\xa0";
  if (!c.bi_list.empty()) {{
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "\xe5\x90\x91\xe4\xb8\x8a" : "\xe5\x90\x91\xe4\xb8\x8b";
  }}
  return make_kline_signal_v1(FreqName(c.freq), "D1", "{opcode}", v1);
}}
'''

# 生成 ang/xl/vol/pressure/byi/coo/jcc/zdy 简单模式
SIMPLE_BODIES = '''
// {module}: simple signal pattern
static std::vector<Signal> {fn}(const CZSC& c, const ParamView& p, TaCache*) {{
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "{opcode}", "\xe5\x85\xb6\xe4\xbb\x96");
}}
'''

# 生成 trader 信号 stubs (pos/cat/zdy_trader/cxt_trader)
TRADER_BODIES = '''
// trader signal: {fn}
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> {fn}(const CZSC&, const ParamView&, TaCache* = nullptr) {{
  return {{}};
}}
'''

# 已实现的信号
IMPLEMENTED = {
    'bar_single_V230506', 'bar_zdt_V230331',
    'tas_ma_base_V221203', 'tas_macd_direct_V221106',
    'cxt_bi_status_V230102', 'cxt_first_buy_V221126',
    'vol_single_ma_V230214',
}

DEFER_IMPLEMENTED = True  # defer real impls from existing code

lines = [HEADER]

# 生成 stubs (skip implemented ones)
for module, sigs in sorted(by_file.items()):
    count = sum(1 for s in sigs if s['name'] not in IMPLEMENTED)
    if count == 0: continue
    lines.append(f"// ============================================================")
    lines.append(f"// {module} signals ({count} stubs)")
    lines.append(f"// ============================================================")

    for s in sigs:
        if s['name'] in IMPLEMENTED: continue
        fn = s['name'].lower()
        if module == 'bar':
            lines.append(BAR_BODIES.format(fn=fn, opcode=s['opcode']))
        elif module == 'tas':
            lines.append(TAS_BODIES.format(fn=fn, opcode=s['opcode'], count=count))
        elif module == 'cxt':
            lines.append(CXT_BODIES.format(fn=fn, opcode=s['opcode'], count=count))
        elif module in ('pos', 'cat', 'zdy_trader', 'cxt_trader'):
            lines.append(TRADER_BODIES.format(fn=fn))
        else:
            lines.append(SIMPLE_BODIES.format(fn=fn, module=module, opcode=s['opcode']))

# 注册表构建
lines.append("")
lines.append("// ============================================================")
lines.append(f"// 注册表构建 ({len(signals)} signals)")
lines.append("// ============================================================")
lines.append("static std::unordered_map<std::string, SignalMeta> build_registry() {")
lines.append("  std::unordered_map<std::string, SignalMeta> m;")
lines.append("")

for s in sorted(signals, key=lambda x: x['name']):
    fn = s['name'].lower()
    cat = 'SignalCategory::kKline' if s['category'] == 'kline' else 'SignalCategory::kTrader'
    tmpl = s['template'].replace('"', '\\"')
    lines.append(f'  m["{s["name"]}"] = {{{cat}, {fn}, "{tmpl}", "{s["opcode"]}"}};')

lines.append("")
lines.append("  return m;")
lines.append("}")
lines.append("")

# 公共 API
lines.append("const std::unordered_map<std::string, SignalMeta>& signal_registry() {")
lines.append("  static auto registry = build_registry();")
lines.append("  return registry;")
lines.append("}")
lines.append("")
lines.append("std::vector<RegisteredSignalInfo> list_all_signals() {")
lines.append("  std::vector<RegisteredSignalInfo> out;")
lines.append("  for (auto& [name, meta] : signal_registry()) {")
lines.append("    std::string cat = (meta.category == SignalCategory::kKline) ? \"kline\" : \"trader\";")
lines.append("    std::string ns = name.substr(0, name.find('_'));")
lines.append("    out.push_back({name, std::string(meta.param_template), cat, ns});")
lines.append("  }")
lines.append("  std::sort(out.begin(), out.end(), [](auto& a, auto& b) {")
lines.append("    if (a.category != b.category) return a.category < b.category;")
lines.append("    return a.name < b.name;")
lines.append("  });")
lines.append("  return out;")
lines.append("}")
lines.append("")
lines.append("std::vector<Signal> run_signal(const char* name, const CZSC& czsc,")
lines.append("                               const ParamView& params, TaCache* cache) {")
lines.append("  auto& reg = signal_registry();")
lines.append("  auto it = reg.find(name);")
lines.append("  if (it == reg.end()) return {};")
lines.append("  return it->second.func(czsc, params, cache);")
lines.append("}")
lines.append("")
lines.append("}  // namespace czsc::signals")

content = '\n'.join(lines)
OUT.write_text(content)
print(f"Generated {OUT} with {len(signals)} signals")
print(f"  kline: {sum(1 for s in signals if s['category']=='kline')}")
print(f"  trader: {sum(1 for s in signals if s['category']=='trader')}")
print(f"  implemented: {len(IMPLEMENTED)}")
print(f"  stubs: {len(signals) - len(IMPLEMENTED)}")
