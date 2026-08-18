// 信号注册表 + 信号函数实现（自动生成 + 手动实现）
// 从 Rust #[signal] 属性生成，246 signals total
// Source: ~/peiking88/czsc/crates/czsc-signals/src/

#include "czsc/signals/registry.hpp"
#include "czsc/signals/signal_builder.hpp"
#include "czsc/ta/indicators.hpp"
#include "czsc/ta/ta_cache.hpp"
#include "czsc/analyze/czsc.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace czsc::signals {

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
static std::string F(size_t n) { return std::to_string(n); }


// ============================================================
// ang signals (10 stubs)
// ============================================================

// ang: simple signal pattern
static std::vector<Signal> adtm_up_dw_line_v230603(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "AdtmUpDwLineV230603", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> amv_up_dw_line_v230603(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "AmvUpDwLineV230603", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> asi_up_dw_line_v230603(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "AsiUpDwLineV230603", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> cmo_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CmoUpDwLineV230605", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> skdj_up_dw_line_v230611(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "SkdjUpDwLineV230611", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> bias_up_dw_line_v230618(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "BiasUpDwLineV230618", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> dema_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "DemaUpDwLineV230605", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> demakder_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "DemakderUpDwLineV230605", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> emv_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "EmvUpDwLineV230605", "å¶ä»");
}


// ang: simple signal pattern
static std::vector<Signal> er_up_dw_line_v230604(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ErUpDwLineV230604", "å¶ä»");
}

// ============================================================
// bar signals (44 stubs)
// ============================================================

// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_triple_v230506(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarTripleV230506", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_end_v221211(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarEndV221211", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_operate_span_v221111(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarOperateSpanV221111", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_time_v230327(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarTimeV230327", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_weekday_v230328(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarWeekdayV230328", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_vol_grow_v221112(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarVolGrowV221112", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_mean_amount_v221112(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarMeanAmountV221112", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_zdf_v221203(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarZdfV221203", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_amount_acc_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarAmountAccV230214", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_single_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarSingleV230214", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_big_solid_v230215(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarBigSolidV230215", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_bpm_v230227(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarBpmV230227", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_section_momentum_v221112(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarSectionMomentumV221112", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_vol_bs1_v230224(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarVolBs1V230224", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_zt_count_v230504(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarZtCountV230504", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_limit_down_v230525(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarLimitDownV230525", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_accelerate_v221110(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarAccelerateV221110", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_accelerate_v221118(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarAccelerateV221118", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_accelerate_v240428(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarAccelerateV240428", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_fake_break_v230204(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarFakeBreakV230204", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_reversal_v230227(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarReversalV230227", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_r_breaker_v230326(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarRBreakerV230326", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_dual_thrust_v230403(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarDualThrustV230403", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_tnr_v230630(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarTnrV230630", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_tnr_v230629(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarTnrV230629", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_shuang_fei_v230507(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarShuangFeiV230507", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_fang_liang_break_v221216(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarFangLiangBreakV221216", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_channel_v230508(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarChannelV230508", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_eight_v230702(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarEightV230702", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_window_std_v230731(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarWindowStdV230731", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_window_ps_v230731(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarWindowPsV230731", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_window_ps_v230801(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarWindowPsV230801", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_trend_v240209(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarTrendV240209", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_plr_v240427(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarPlrV240427", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_polyfit_v240428(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarPolyfitV240428", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_break_v240428(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarBreakV240428", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_classify_v240606(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarClassifyV240606", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_classify_v240607(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarClassifyV240607", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_decision_v240608(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarDecisionV240608", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_decision_v240616(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarDecisionV240616", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_td9_v240616(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarTd9V240616", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_volatility_v241013(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarVolatilityV241013", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_zfzd_v241013(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarZfzdV241013", "å¶ä»");
}


// ---- bar: 单K/多K形态识别 (46) ----
// bar_single_V230506 — 已实现
// bar_zdt_V230331 — 已实现
static std::vector<Signal> bar_zfzd_v241014(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(p.usize("di",1)), "BarZfzdV241014", "å¶ä»");
}

// ============================================================
// byi signals (5 stubs)
// ============================================================

// byi: simple signal pattern
static std::vector<Signal> byi_symmetry_zs_v221107(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ByiSymmetryZsV221107", "å¶ä»");
}


// byi: simple signal pattern
static std::vector<Signal> byi_bi_end_v230106(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ByiBiEndV230106", "å¶ä»");
}


// byi: simple signal pattern
static std::vector<Signal> byi_bi_end_v230107(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ByiBiEndV230107", "å¶ä»");
}


// byi: simple signal pattern
static std::vector<Signal> byi_second_bs_v230324(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ByiSecondBsV230324", "å¶ä»");
}


// byi: simple signal pattern
static std::vector<Signal> byi_fx_num_v230628(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ByiFxNumV230628", "å¶ä»");
}

// ============================================================
// cat signals (2 stubs)
// ============================================================

// trader signal: cat_macd_v230518
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> cat_macd_v230518(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: cat_macd_v230520
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> cat_macd_v230520(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}

// ============================================================
// clv signals (1 stubs)
// ============================================================

// clv: simple signal pattern
static std::vector<Signal> clv_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ClvUpDwLineV230605", "å¶ä»");
}

// ============================================================
// coo signals (5 stubs)
// ============================================================

// coo: simple signal pattern
static std::vector<Signal> coo_td_v221110(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CooTdV221110", "å¶ä»");
}


// coo: simple signal pattern
static std::vector<Signal> coo_td_v221111(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CooTdV221111", "å¶ä»");
}


// coo: simple signal pattern
static std::vector<Signal> coo_cci_v230323(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CooCciV230323", "å¶ä»");
}


// coo: simple signal pattern
static std::vector<Signal> coo_kdj_v230322(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CooKdjV230322", "å¶ä»");
}


// coo: simple signal pattern
static std::vector<Signal> coo_sar_v230325(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CooSarV230325", "å¶ä»");
}

// ============================================================
// cvolp signals (1 stubs)
// ============================================================

// cvolp: simple signal pattern
static std::vector<Signal> cvolp_up_dw_line_v230612(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "CvolpUpDwLineV230612", "å¶ä»");
}

// ============================================================
// cxt signals (39 stubs)
// ============================================================

// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_base_v230228(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiBaseV230228", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_status_v230101(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiStatusV230101", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_fx_power_v221107(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtFxPowerV221107", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230104(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230104", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230105(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230105", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230224(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230224", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230312(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230312", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230324(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230324", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230815(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230815", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_stop_v230815(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiStopV230815", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_trend_v230824(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiTrendV230824", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_zdf_v230601(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiZdfV230601", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_second_bs_v230320(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtSecondBsV230320", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_third_bs_v230318(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtThirdBsV230318", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_double_zs_v230311(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtDoubleZsV230311", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_overlap_v240526(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtOverlapV240526", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_decision_v240526(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtDecisionV240526", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_decision_v240612(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtDecisionV240612", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_decision_v240613(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtDecisionV240613", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_decision_v240614(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtDecisionV240614", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bs_v240526(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBsV240526", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bs_v240527(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBsV240527", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_first_sell_v221126(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtFirstSellV221126", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230222(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230222", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_third_buy_v230228(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtThirdBuyV230228", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_third_bs_v230319(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtThirdBsV230319", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230320(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230320", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230322(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230322", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_end_v230618(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiEndV230618", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_three_bi_v230618(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtThreeBiV230618", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_five_bi_v230619(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtFiveBiV230619", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_seven_bi_v230620(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtSevenBiV230620", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_range_oscillation_v230620(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtRangeOscillationV230620", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_nine_bi_v230621(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtNineBiV230621", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_eleven_bi_v230622(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtElevenBiV230622", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_ubi_end_v230816(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtUbiEndV230816", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_bi_trend_v230913(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtBiTrendV230913", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_second_bs_v240524(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtSecondBsV240524", v1);
}


// cxt: 缠论上下文 (39 个)
static std::vector<Signal> cxt_overlap_v240612(const CZSC& c, const ParamView& p, TaCache*) {
  std::string v1 = "æ ";
  if (!c.bi_list.empty()) {
    auto& last = c.bi_list.back();
    v1 = (last.direction == Direction::kUp) ? "åä¸" : "åä¸";
  }
  return make_kline_signal_v1(FreqName(c.freq), "D1", "CxtOverlapV240612", v1);
}

// ============================================================
// cxt_trader signals (2 stubs)
// ============================================================

// trader signal: cxt_zhong_shu_gong_zhen_v221221
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> cxt_zhong_shu_gong_zhen_v221221(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: cxt_intraday_v230701
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> cxt_intraday_v230701(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}

// ============================================================
// jcc signals (19 stubs)
// ============================================================

// jcc: simple signal pattern
static std::vector<Signal> jcc_san_xing_xian_v221023(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccSanXingXianV221023", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_ten_mo_v221028(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccTenMoV221028", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_wu_yun_gai_ding_v221101(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccWuYunGaiDingV221101", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_ci_tou_v221101(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccCiTouV221101", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_san_fa_v20221118(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccSanFaV20221118", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_san_fa_v20221115(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccSanFaV20221115", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_xing_xian_v221118(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccXingXianV221118", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_fen_shou_xian_v20221113(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccFenShouXianV20221113", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_zhu_huo_xian_v221027(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccZhuHuoXianV221027", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_yun_xian_v221118(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccYunXianV221118", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_ping_tou_v221113(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccPingTouV221113", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_two_crow_v221108(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccTwoCrowV221108", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_three_crow_v221108(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccThreeCrowV221108", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_szx_v221111(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccSzxV221111", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_san_szx_v221122(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccSanSzxV221122", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_fan_ji_xian_v221121(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccFanJiXianV221121", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_shan_chun_v221121(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccShanChunV221121", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_gap_yin_yang_v221121(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccGapYinYangV221121", "å¶ä»");
}


// jcc: simple signal pattern
static std::vector<Signal> jcc_ta_xing_v221124(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "JccTaXingV221124", "å¶ä»");
}

// ============================================================
// kcatr signals (1 stubs)
// ============================================================

// kcatr: simple signal pattern
static std::vector<Signal> kcatr_up_dw_line_v230823(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "KcatrUpDwLineV230823", "å¶ä»");
}

// ============================================================
// ntmdk signals (1 stubs)
// ============================================================

// ntmdk: simple signal pattern
static std::vector<Signal> ntmdk_v230824(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "NtmdkV230824", "å¶ä»");
}

// ============================================================
// obv signals (2 stubs)
// ============================================================

// obv: simple signal pattern
static std::vector<Signal> obvm_line_v230610(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ObvmLineV230610", "å¶ä»");
}


// obv: simple signal pattern
static std::vector<Signal> obv_up_dw_line_v230719(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ObvUpDwLineV230719", "å¶ä»");
}

// ============================================================
// pos signals (16 stubs)
// ============================================================

// trader signal: pos_ma_v230414
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_ma_v230414(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_fx_stop_v230414
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_fx_stop_v230414(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_bar_stop_v230524
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_bar_stop_v230524(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_holds_v230414
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_holds_v230414(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_fix_exit_v230624
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_fix_exit_v230624(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_profit_loss_v230624
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_profit_loss_v230624(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_status_v230808
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_status_v230808(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_holds_v230807
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_holds_v230807(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_holds_v240428
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_holds_v240428(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_holds_v240608
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_holds_v240608(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_stop_v240428
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_stop_v240428(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_take_v240428
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_take_v240428(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_stop_v240331
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_stop_v240331(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_stop_v240608
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_stop_v240608(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_stop_v240614
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_stop_v240614(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: pos_stop_v240717
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> pos_stop_v240717(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}

// ============================================================
// pressure signals (4 stubs)
// ============================================================

// pressure: simple signal pattern
static std::vector<Signal> pressure_support_v240222(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "PressureSupportV240222", "å¶ä»");
}


// pressure: simple signal pattern
static std::vector<Signal> pressure_support_v240402(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "PressureSupportV240402", "å¶ä»");
}


// pressure: simple signal pattern
static std::vector<Signal> pressure_support_v240406(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "PressureSupportV240406", "å¶ä»");
}


// pressure: simple signal pattern
static std::vector<Signal> pressure_support_v240530(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "PressureSupportV240530", "å¶ä»");
}

// ============================================================
// tas signals (57 stubs)
// ============================================================

// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_ma_base_v221101(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMaBaseV221101", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_ma_base_v230313(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMaBaseV230313", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_ma_round_v221206(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMaRoundV221206", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_double_ma_v230511(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDoubleMaV230511", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_base_v221028(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBaseV221028", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_power_v221108(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdPowerV221108", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_first_bs_v230217(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasFirstBsV230217", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_second_bs_v230228(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasSecondBsV230228", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_second_bs_v230303(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasSecondBsV230303", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_hlma_v230301(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasHlmaV230301", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_boll_cc_v230312(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasBollCcV230312", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_kdj_evc_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasKdjEvcV221201", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_kdj_evc_v230401(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasKdjEvcV230401", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_atr_break_v230424(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasAtrBreakV230424", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_ma_system_v230513(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMaSystemV230513", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_ma_cohere_v230512(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMaCohereV230512", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_dif_layer_v241010(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDifLayerV241010", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_cross_status_v230619(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasCrossStatusV230619", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_cross_status_v230624(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasCrossStatusV230624", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_cross_status_v230625(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasCrossStatusV230625", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_slope_v231019(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasSlopeV231019", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_boll_vt_v230212(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasBollVtV230212", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_cci_base_v230402(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasCciBaseV230402", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> cci_decision_v240620(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "CciDecisionV240620", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_accelerate_v230531(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasAccelerateV230531", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_low_trend_v230627(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasLowTrendV230627", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_atr_v230630(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasAtrV230630", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_base_v230320(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBaseV230320", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_change_v221105(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdChangeV221105", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_dif_zero_v240614(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDifZeroV240614", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_dif_zero_v240612(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDifZeroV240612", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bc_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBcV221201", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_angle_v230802(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasAngleV230802", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_double_ma_v240208(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDoubleMaV240208", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_dma_bs_v240608(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDmaBsV240608", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bc_v230803(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBcV230803", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bc_v240307(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBcV240307", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_dist_v230408(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdDistV230408", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_dist_v230409(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdDistV230409", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_dist_v230410(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdDistV230410", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_first_bs_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdFirstBsV221201", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_first_bs_v221216(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdFirstBsV221216", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_second_bs_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdSecondBsV221201", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_xt_v221208(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdXtV221208", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bs1_v230312(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBs1V230312", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bs1_v230313(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBs1V230313", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_boll_power_v221112(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasBollPowerV221112", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_boll_bc_v221118(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasBollBcV221118", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_kdj_base_v221101(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasKdjBaseV221101", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_rsi_base_v230227(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasRsiBaseV230227", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_double_ma_v221203(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasDoubleMaV221203", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_sar_base_v230425(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasSarBaseV230425", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bs1_v230411(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBs1V230411", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bs1_v230412(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBs1V230412", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_rumi_v230704(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasRumiV230704", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bc_v230804(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBcV230804", "å¶ä»");
}


// tas: TA指标信号 (57 个)
static std::vector<Signal> tas_macd_bc_ubi_v230804(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache) return {};
  size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 5);
  if (c.bars_raw.size() < tp + 3) return {};
  return make_kline_signal_v1(FreqName(c.freq),
      "D" + F(di) + "#" + F(tp), "TasMacdBcUbiV230804", "å¶ä»");
}

// ============================================================
// vol signals (5 stubs)
// ============================================================

// vol: simple signal pattern
static std::vector<Signal> vol_double_ma_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "VolDoubleMaV230214", "å¶ä»");
}


// vol: simple signal pattern
static std::vector<Signal> vol_ti_suo_v221216(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "VolTiSuoV221216", "å¶ä»");
}


// vol: simple signal pattern
static std::vector<Signal> vol_gao_di_v221218(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "VolGaoDiV221218", "å¶ä»");
}


// vol: simple signal pattern
static std::vector<Signal> vol_window_v230731(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "VolWindowV230731", "å¶ä»");
}


// vol: simple signal pattern
static std::vector<Signal> vol_window_v230801(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "VolWindowV230801", "å¶ä»");
}

// ============================================================
// xl signals (7 stubs)
// ============================================================

// xl: simple signal pattern
static std::vector<Signal> xl_bar_position_v240328(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarPositionV240328", "å¶ä»");
}


// xl: simple signal pattern
static std::vector<Signal> xl_bar_trend_v240329(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarTrendV240329", "å¶ä»");
}


// xl: simple signal pattern
static std::vector<Signal> xl_bar_trend_v240330(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarTrendV240330", "å¶ä»");
}


// xl: simple signal pattern
static std::vector<Signal> xl_bar_trend_v240331(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarTrendV240331", "å¶ä»");
}


// xl: simple signal pattern
static std::vector<Signal> xl_bar_basis_v240412(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarBasisV240412", "å¶ä»");
}


// xl: simple signal pattern
static std::vector<Signal> xl_bar_basis_v240411(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarBasisV240411", "å¶ä»");
}


// xl: simple signal pattern
static std::vector<Signal> xl_bar_trend_v240623(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "XlBarTrendV240623", "å¶ä»");
}

// ============================================================
// zdy signals (14 stubs)
// ============================================================

// zdy: simple signal pattern
static std::vector<Signal> zdy_bi_end_v230406(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyBiEndV230406", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_bi_end_v230407(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyBiEndV230407", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_zs_v230423(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyZsV230423", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_zs_space_v230421(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyZsSpaceV230421", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_bc_v230422(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdBcV230422", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_bs1_v230422(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdBs1V230422", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_dif_v230516(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdDifV230516", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_dif_v230517(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdDifV230517", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_v230518(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdV230518", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_v230519(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdV230519", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_dif_iqr_v230521(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdDifIqrV230521", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_macd_v230527(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyMacdV230527", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_dif_v230527(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyDifV230527", "å¶ä»");
}


// zdy: simple signal pattern
static std::vector<Signal> zdy_dif_v230528(const CZSC& c, const ParamView& p, TaCache*) {
  return make_kline_signal_v1(FreqName(c.freq),
      "D1", "ZdyDifV230528", "å¶ä»");
}

// ============================================================
// zdy_trader signals (4 stubs)
// ============================================================

// trader signal: zdy_vibrate_v230406
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> zdy_vibrate_v230406(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: zdy_stop_loss_v230406
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> zdy_stop_loss_v230406(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: zdy_take_profit_v230406
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> zdy_take_profit_v230406(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// trader signal: zdy_take_profit_v230407
// Note: trader signals require TraderState which is not migrated yet
static std::vector<Signal> zdy_take_profit_v230407(const CZSC&, const ParamView&, TaCache* = nullptr) {
  return {};
}


// ============================================================
// 注册表构建 (246 signals)
// ============================================================
static std::unordered_map<std::string, SignalMeta> build_registry() {
  std::unordered_map<std::string, SignalMeta> m;

  m["adtm_up_dw_line_V230603"] = {SignalCategory::kKline, adtm_up_dw_line_v230603, "{freq}_D{di}N{n}M{m}TH{th}_ADTMV230603", "AdtmUpDwLineV230603"};
  m["amv_up_dw_line_V230603"] = {SignalCategory::kKline, amv_up_dw_line_v230603, "{freq}_D{di}N{n}M{m}_AMV能量V230603", "AmvUpDwLineV230603"};
  m["asi_up_dw_line_V230603"] = {SignalCategory::kKline, asi_up_dw_line_v230603, "{freq}_D{di}N{n}P{p}_ASI多空V230603", "AsiUpDwLineV230603"};
  m["bar_accelerate_V221110"] = {SignalCategory::kKline, bar_accelerate_v221110, "{freq}_D{di}W{window}_加速V221110", "BarAccelerateV221110"};
  m["bar_accelerate_V221118"] = {SignalCategory::kKline, bar_accelerate_v221118, "{freq}_D{di}W{window}#{ma_type}#{timeperiod}_加速V221118", "BarAccelerateV221118"};
  m["bar_accelerate_V240428"] = {SignalCategory::kKline, bar_accelerate_v240428, "{freq}_D{di}W{w}T{t}_加速V240428", "BarAccelerateV240428"};
  m["bar_amount_acc_V230214"] = {SignalCategory::kKline, bar_amount_acc_v230214, "{freq}_D{di}N{n}_累计超{t}千万V230214", "BarAmountAccV230214"};
  m["bar_big_solid_V230215"] = {SignalCategory::kKline, bar_big_solid_v230215, "{freq}_D{di}N{n}_MIDV230215", "BarBigSolidV230215"};
  m["bar_bpm_V230227"] = {SignalCategory::kKline, bar_bpm_v230227, "{freq}_D{di}N{n}T{th}_绝对动量V230227", "BarBpmV230227"};
  m["bar_break_V240428"] = {SignalCategory::kKline, bar_break_v240428, "{freq}_D{di}W{w}_事件V240428", "BarBreakV240428"};
  m["bar_channel_V230508"] = {SignalCategory::kKline, bar_channel_v230508, "{freq}_D{di}M{m}_通道V230508", "BarChannelV230508"};
  m["bar_classify_V240606"] = {SignalCategory::kKline, bar_classify_v240606, "{freq}_D{di}收盘位置_分类V240606", "BarClassifyV240606"};
  m["bar_classify_V240607"] = {SignalCategory::kKline, bar_classify_v240607, "{freq}_D{di}K2收盘位置_分类V240607", "BarClassifyV240607"};
  m["bar_decision_V240608"] = {SignalCategory::kKline, bar_decision_v240608, "{freq}_W{w}N{n}Q{q}放量_决策区域V240608", "BarDecisionV240608"};
  m["bar_decision_V240616"] = {SignalCategory::kKline, bar_decision_v240616, "{freq}_W{w}N{n}强弱_决策区域V240616", "BarDecisionV240616"};
  m["bar_dual_thrust_V230403"] = {SignalCategory::kKline, bar_dual_thrust_v230403, "{freq}_D{di}通道突破#{N}#{K1}#{K2}_BS辅助V230403", "BarDualThrustV230403"};
  m["bar_eight_V230702"] = {SignalCategory::kKline, bar_eight_v230702, "{freq}_D{di}#8K_走势分类V230702", "BarEightV230702"};
  m["bar_end_V221211"] = {SignalCategory::kKline, bar_end_v221211, "{freq}_{freq1}结束_BS辅助221211", "BarEndV221211"};
  m["bar_fake_break_V230204"] = {SignalCategory::kKline, bar_fake_break_v230204, "{freq}_D{di}N{n}M{m}_假突破V230204", "BarFakeBreakV230204"};
  m["bar_fang_liang_break_V221216"] = {SignalCategory::kKline, bar_fang_liang_break_v221216, "{freq}_D{di}TH{th}#{ma_type}#{timeperiod}_突破V221216", "BarFangLiangBreakV221216"};
  m["bar_limit_down_V230525"] = {SignalCategory::kKline, bar_limit_down_v230525, "{freq}_跌停后无下影线长实体阳线_短线V230525", "BarLimitDownV230525"};
  m["bar_mean_amount_V221112"] = {SignalCategory::kKline, bar_mean_amount_v221112, "{freq}_D{di}K{n}B均额_{th1}至{th2}千万V221112", "BarMeanAmountV221112"};
  m["bar_operate_span_V221111"] = {SignalCategory::kKline, bar_operate_span_v221111, "{freq}_T{t1}#{t2}_时间区间V221111", "BarOperateSpanV221111"};
  m["bar_plr_V240427"] = {SignalCategory::kKline, bar_plr_v240427, "{freq}_D{di}W{w}T{t}M{m}_盈亏比V240427", "BarPlrV240427"};
  m["bar_polyfit_V240428"] = {SignalCategory::kKline, bar_polyfit_v240428, "{freq}_D{di}W{w}_分类V240428", "BarPolyfitV240428"};
  m["bar_r_breaker_V230326"] = {SignalCategory::kKline, bar_r_breaker_v230326, "{freq}_RBreaker_BS辅助V230326", "BarRBreakerV230326"};
  m["bar_reversal_V230227"] = {SignalCategory::kKline, bar_reversal_v230227, "{freq}_D{di}A{avg_bp}_反转V230227", "BarReversalV230227"};
  m["bar_section_momentum_V221112"] = {SignalCategory::kKline, bar_section_momentum_v221112, "{freq}_D{di}K{n}B_阈值{th}BPV221112", "BarSectionMomentumV221112"};
  m["bar_shuang_fei_V230507"] = {SignalCategory::kKline, bar_shuang_fei_v230507, "{freq}_D{di}双飞_短线V230507", "BarShuangFeiV230507"};
  m["bar_single_V230214"] = {SignalCategory::kKline, bar_single_v230214, "{freq}_D{di}T{t}_状态V230214", "BarSingleV230214"};
  m["bar_single_V230506"] = {SignalCategory::kKline, bar_single_v230506, "{freq}_D{di}单K趋势N{n}_BS辅助V230506", "BarSingleV230506"};
  m["bar_td9_V240616"] = {SignalCategory::kKline, bar_td9_v240616, "{freq}_神奇九转N{n}_BS辅助V240616", "BarTd9V240616"};
  m["bar_time_V230327"] = {SignalCategory::kKline, bar_time_v230327, "{freq}_日内时间_分段V230327", "BarTimeV230327"};
  m["bar_tnr_V230629"] = {SignalCategory::kKline, bar_tnr_v230629, "{freq}_D{di}TNR{timeperiod}_趋势V230629", "BarTnrV230629"};
  m["bar_tnr_V230630"] = {SignalCategory::kKline, bar_tnr_v230630, "{freq}_D{di}TNR{timeperiod}K{k}_趋势V230630", "BarTnrV230630"};
  m["bar_trend_V240209"] = {SignalCategory::kKline, bar_trend_v240209, "{freq}_D{di}N{N}趋势跟踪_BS辅助V240209", "BarTrendV240209"};
  m["bar_triple_V230506"] = {SignalCategory::kKline, bar_triple_v230506, "{freq}_D{di}三K加速_裸K形态V230506", "BarTripleV230506"};
  m["bar_vol_bs1_V230224"] = {SignalCategory::kKline, bar_vol_bs1_v230224, "{freq}_D{di}N{n}量价_BS1辅助V230224", "BarVolBs1V230224"};
  m["bar_vol_grow_V221112"] = {SignalCategory::kKline, bar_vol_grow_v221112, "{freq}_D{di}K{n}B_放量V221112", "BarVolGrowV221112"};
  m["bar_volatility_V241013"] = {SignalCategory::kKline, bar_volatility_v241013, "{freq}_波动率分层W{w}N{n}_完全分类V241013", "BarVolatilityV241013"};
  m["bar_weekday_V230328"] = {SignalCategory::kKline, bar_weekday_v230328, "{freq}_周内时间_分段V230328", "BarWeekdayV230328"};
  m["bar_window_ps_V230731"] = {SignalCategory::kKline, bar_window_ps_v230731, "{freq}_W{w}M{m}N{n}L{l}_支撑压力位V230731", "BarWindowPsV230731"};
  m["bar_window_ps_V230801"] = {SignalCategory::kKline, bar_window_ps_v230801, "{freq}_N{n}W{w}_支撑压力位V230801", "BarWindowPsV230801"};
  m["bar_window_std_V230731"] = {SignalCategory::kKline, bar_window_std_v230731, "{freq}_D{di}W{w}M{m}N{n}_窗口波动V230731", "BarWindowStdV230731"};
  m["bar_zdf_V221203"] = {SignalCategory::kKline, bar_zdf_v221203, "{freq}_D{di}{mode}_{t1}至{t2}V221203", "BarZdfV221203"};
  m["bar_zdt_V230331"] = {SignalCategory::kKline, bar_zdt_v230331, "{freq}_D{di}_涨跌停V230331", "BarZdtV230331"};
  m["bar_zfzd_V241013"] = {SignalCategory::kKline, bar_zfzd_v241013, "{freq}_窄幅震荡N{n}_形态V241013", "BarZfzdV241013"};
  m["bar_zfzd_V241014"] = {SignalCategory::kKline, bar_zfzd_v241014, "{freq}_窄幅震荡N{n}_形态V241014", "BarZfzdV241014"};
  m["bar_zt_count_V230504"] = {SignalCategory::kKline, bar_zt_count_v230504, "{freq}_D{di}W{window}涨停计数_裸K形态V230504", "BarZtCountV230504"};
  m["bias_up_dw_line_V230618"] = {SignalCategory::kKline, bias_up_dw_line_v230618, "{freq}_D{di}N{n}M{m}P{p}TH1{th1}TH2{th2}TH3{th3}_BIAS乖离率V230618", "BiasUpDwLineV230618"};
  m["byi_bi_end_V230106"] = {SignalCategory::kKline, byi_bi_end_v230106, "{freq}_D0停顿分型_BE辅助V230106", "ByiBiEndV230106"};
  m["byi_bi_end_V230107"] = {SignalCategory::kKline, byi_bi_end_v230107, "{freq}_D0验证分型_BE辅助V230107", "ByiBiEndV230107"};
  m["byi_fx_num_V230628"] = {SignalCategory::kKline, byi_fx_num_v230628, "{freq}_D{di}笔分型数大于{num}_BE辅助V230628", "ByiFxNumV230628"};
  m["byi_second_bs_V230324"] = {SignalCategory::kKline, byi_second_bs_v230324, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}回抽零轴_BS2辅助V230324", "ByiSecondBsV230324"};
  m["byi_symmetry_zs_V221107"] = {SignalCategory::kKline, byi_symmetry_zs_v221107, "{freq}_D{di}B_对称中枢V221107", "ByiSymmetryZsV221107"};
  m["cat_macd_V230518"] = {SignalCategory::kTrader, cat_macd_v230518, "{freq1}#{freq2}_MACD交叉_联立V230518", "CatMacdV230518"};
  m["cat_macd_V230520"] = {SignalCategory::kTrader, cat_macd_v230520, "{freq1}#{freq2}_MACD交叉_联立V230520", "CatMacdV230520"};
  m["cci_decision_V240620"] = {SignalCategory::kKline, cci_decision_v240620, "{freq}_N{n}CCI_决策区域V240620", "CciDecisionV240620"};
  m["clv_up_dw_line_V230605"] = {SignalCategory::kKline, clv_up_dw_line_v230605, "{freq}_D{di}N{n}_CLV多空V230605", "ClvUpDwLineV230605"};
  m["cmo_up_dw_line_V230605"] = {SignalCategory::kKline, cmo_up_dw_line_v230605, "{freq}_D{di}N{n}M{m}_CMO能量V230605", "CmoUpDwLineV230605"};
  m["coo_cci_V230323"] = {SignalCategory::kKline, coo_cci_v230323, "{freq}_D{di}CCI{n}#{ma_type}#{m}_BS辅助V230323", "CooCciV230323"};
  m["coo_kdj_V230322"] = {SignalCategory::kKline, coo_kdj_v230322, "{freq}_D{di}KDJ{fastk_period}#{slowk_period}#{slowd_period}#{ma_type}#{n}_BS辅助V230322", "CooKdjV230322"};
  m["coo_sar_V230325"] = {SignalCategory::kKline, coo_sar_v230325, "{freq}_D{di}N{n}SAR_BS辅助V230325", "CooSarV230325"};
  m["coo_td_V221110"] = {SignalCategory::kKline, coo_td_v221110, "{freq}_D{di}K_TDV221110", "CooTdV221110"};
  m["coo_td_V221111"] = {SignalCategory::kKline, coo_td_v221111, "{freq}_D{di}TD_BS辅助V221111", "CooTdV221111"};
  m["cvolp_up_dw_line_V230612"] = {SignalCategory::kKline, cvolp_up_dw_line_v230612, "{freq}_D{di}N{n}M{m}UP{up}DW{dw}_CVOLP动量变化率V230612", "CvolpUpDwLineV230612"};
  m["cxt_bi_base_V230228"] = {SignalCategory::kKline, cxt_bi_base_v230228, "{freq}_D0BL{bi_init_length}_V230228", "CxtBiBaseV230228"};
  m["cxt_bi_end_V230104"] = {SignalCategory::kKline, cxt_bi_end_v230104, "{freq}_D0{ma_type}#{timeperiod}T{th}_BE辅助V230104", "CxtBiEndV230104"};
  m["cxt_bi_end_V230105"] = {SignalCategory::kKline, cxt_bi_end_v230105, "{freq}_D0{ma_type}#{timeperiod}T{th}_BE辅助V230105", "CxtBiEndV230105"};
  m["cxt_bi_end_V230222"] = {SignalCategory::kKline, cxt_bi_end_v230222, "{freq}_D1MO{max_overlap}_BE辅助V230222", "CxtBiEndV230222"};
  m["cxt_bi_end_V230224"] = {SignalCategory::kKline, cxt_bi_end_v230224, "{freq}_D1_BE辅助V230224", "CxtBiEndV230224"};
  m["cxt_bi_end_V230312"] = {SignalCategory::kKline, cxt_bi_end_v230312, "{freq}_D0MACD{fastperiod}#{slowperiod}#{signalperiod}_BE辅助V230312", "CxtBiEndV230312"};
  m["cxt_bi_end_V230320"] = {SignalCategory::kKline, cxt_bi_end_v230320, "{freq}_D0质数窗口MO{max_overlap}_BE辅助V230320", "CxtBiEndV230320"};
  m["cxt_bi_end_V230322"] = {SignalCategory::kKline, cxt_bi_end_v230322, "{freq}_D0分型配合{ma_type}#{timeperiod}_BE辅助V230322", "CxtBiEndV230322"};
  m["cxt_bi_end_V230324"] = {SignalCategory::kKline, cxt_bi_end_v230324, "{freq}_D0{ma_type}#{timeperiod}均线突破_BE辅助V230324", "CxtBiEndV230324"};
  m["cxt_bi_end_V230618"] = {SignalCategory::kKline, cxt_bi_end_v230618, "{freq}_D{di}MO{max_overlap}_BE辅助V230618", "CxtBiEndV230618"};
  m["cxt_bi_end_V230815"] = {SignalCategory::kKline, cxt_bi_end_v230815, "{freq}_快速突破_BE辅助V230815", "CxtBiEndV230815"};
  m["cxt_bi_status_V230101"] = {SignalCategory::kKline, cxt_bi_status_v230101, "{freq}_D1_表里关系V230101", "CxtBiStatusV230101"};
  m["cxt_bi_status_V230102"] = {SignalCategory::kKline, cxt_bi_status_v230102, "{freq}_D1_表里关系V230102", "CxtBiStatusV230102"};
  m["cxt_bi_stop_V230815"] = {SignalCategory::kKline, cxt_bi_stop_v230815, "{freq}_距离{th}BP_止损V230815", "CxtBiStopV230815"};
  m["cxt_bi_trend_V230824"] = {SignalCategory::kKline, cxt_bi_trend_v230824, "{freq}_D{di}N{n}TH{th}_形态V230824", "CxtBiTrendV230824"};
  m["cxt_bi_trend_V230913"] = {SignalCategory::kKline, cxt_bi_trend_v230913, "{freq}_D{di}N{n}笔趋势_高低点辅助判断V230913", "CxtBiTrendV230913"};
  m["cxt_bi_zdf_V230601"] = {SignalCategory::kKline, cxt_bi_zdf_v230601, "{freq}_D{di}N{n}_分层V230601", "CxtBiZdfV230601"};
  m["cxt_bs_V240526"] = {SignalCategory::kKline, cxt_bs_v240526, "{freq}_趋势跟随_BS辅助V240526", "CxtBsV240526"};
  m["cxt_bs_V240527"] = {SignalCategory::kKline, cxt_bs_v240527, "{freq}_趋势跟随_BS辅助V240527", "CxtBsV240527"};
  m["cxt_decision_V240526"] = {SignalCategory::kKline, cxt_decision_v240526, "{freq}_分型区域N{n}_决策区域V240526", "CxtDecisionV240526"};
  m["cxt_decision_V240612"] = {SignalCategory::kKline, cxt_decision_v240612, "{freq}_W{w}N{n}高低点_决策区域V240612", "CxtDecisionV240612"};
  m["cxt_decision_V240613"] = {SignalCategory::kKline, cxt_decision_v240613, "{freq}_放量笔N{n}BS2_决策区域V240613", "CxtDecisionV240613"};
  m["cxt_decision_V240614"] = {SignalCategory::kKline, cxt_decision_v240614, "{freq}_放量笔N{n}_决策区域V240614", "CxtDecisionV240614"};
  m["cxt_double_zs_V230311"] = {SignalCategory::kKline, cxt_double_zs_v230311, "{freq}_D{di}双中枢_BS1辅助V230311", "CxtDoubleZsV230311"};
  m["cxt_eleven_bi_V230622"] = {SignalCategory::kKline, cxt_eleven_bi_v230622, "{freq}_D{di}十一笔_形态V230622", "CxtElevenBiV230622"};
  m["cxt_first_buy_V221126"] = {SignalCategory::kKline, cxt_first_buy_v221126, "{freq}_D{di}B_BUY1V221126", "CxtFirstBuyV221126"};
  m["cxt_first_sell_V221126"] = {SignalCategory::kKline, cxt_first_sell_v221126, "{freq}_D{di}B_SELL1V221126", "CxtFirstSellV221126"};
  m["cxt_five_bi_V230619"] = {SignalCategory::kKline, cxt_five_bi_v230619, "{freq}_D{di}五笔_形态V230619", "CxtFiveBiV230619"};
  m["cxt_fx_power_V221107"] = {SignalCategory::kKline, cxt_fx_power_v221107, "{freq}_D{di}F_分型强弱V221107", "CxtFxPowerV221107"};
  m["cxt_intraday_V230701"] = {SignalCategory::kTrader, cxt_intraday_v230701, "{freq1}#{freq2}_D{di}日_走势分类V230701", "CxtIntradayV230701"};
  m["cxt_nine_bi_V230621"] = {SignalCategory::kKline, cxt_nine_bi_v230621, "{freq}_D{di}九笔_形态V230621", "CxtNineBiV230621"};
  m["cxt_overlap_V240526"] = {SignalCategory::kKline, cxt_overlap_v240526, "{freq}_顶底重合_支撑压力V240526", "CxtOverlapV240526"};
  m["cxt_overlap_V240612"] = {SignalCategory::kKline, cxt_overlap_v240612, "{freq}_SNR顺畅N{n}_支撑压力V240612", "CxtOverlapV240612"};
  m["cxt_range_oscillation_V230620"] = {SignalCategory::kKline, cxt_range_oscillation_v230620, "{freq}_D{di}TH{th}_区间震荡V230620", "CxtRangeOscillationV230620"};
  m["cxt_second_bs_V230320"] = {SignalCategory::kKline, cxt_second_bs_v230320, "{freq}_D{di}#{ma_type}#{timeperiod}_BS2辅助V230320", "CxtSecondBsV230320"};
  m["cxt_second_bs_V240524"] = {SignalCategory::kKline, cxt_second_bs_v240524, "{freq}_D{di}W{w}T{t}_第二买卖点V240524", "CxtSecondBsV240524"};
  m["cxt_seven_bi_V230620"] = {SignalCategory::kKline, cxt_seven_bi_v230620, "{freq}_D{di}七笔_形态V230620", "CxtSevenBiV230620"};
  m["cxt_third_bs_V230318"] = {SignalCategory::kKline, cxt_third_bs_v230318, "{freq}_D{di}#{ma_type}#{timeperiod}_BS3辅助V230318", "CxtThirdBsV230318"};
  m["cxt_third_bs_V230319"] = {SignalCategory::kKline, cxt_third_bs_v230319, "{freq}_D{di}#{ma_type}#{timeperiod}_BS3辅助V230319", "CxtThirdBsV230319"};
  m["cxt_third_buy_V230228"] = {SignalCategory::kKline, cxt_third_buy_v230228, "{freq}_D{di}_三买辅助V230228", "CxtThirdBuyV230228"};
  m["cxt_three_bi_V230618"] = {SignalCategory::kKline, cxt_three_bi_v230618, "{freq}_D{di}三笔_形态V230618", "CxtThreeBiV230618"};
  m["cxt_ubi_end_V230816"] = {SignalCategory::kKline, cxt_ubi_end_v230816, "{freq}_UBI_BE辅助V230816", "CxtUbiEndV230816"};
  m["cxt_zhong_shu_gong_zhen_V221221"] = {SignalCategory::kTrader, cxt_zhong_shu_gong_zhen_v221221, "{freq1}_{freq2}_中枢共振V221221", "CxtZhongShuGongZhenV221221"};
  m["dema_up_dw_line_V230605"] = {SignalCategory::kKline, dema_up_dw_line_v230605, "{freq}_D{di}N{n}_DEMA短线趋势V230605", "DemaUpDwLineV230605"};
  m["demakder_up_dw_line_V230605"] = {SignalCategory::kKline, demakder_up_dw_line_v230605, "{freq}_D{di}N{n}TH{th}TL{tl}_DEMAKER价格趋势V230605", "DemakderUpDwLineV230605"};
  m["emv_up_dw_line_V230605"] = {SignalCategory::kKline, emv_up_dw_line_v230605, "{freq}_D{di}_EMV简易波动V230605", "EmvUpDwLineV230605"};
  m["er_up_dw_line_V230604"] = {SignalCategory::kKline, er_up_dw_line_v230604, "{freq}_D{di}W{w}N{n}_ER价格动量V230604", "ErUpDwLineV230604"};
  m["jcc_ci_tou_V221101"] = {SignalCategory::kKline, jcc_ci_tou_v221101, "{freq}_D{di}Z{z}TH{th}_刺透形态V221101", "JccCiTouV221101"};
  m["jcc_fan_ji_xian_V221121"] = {SignalCategory::kKline, jcc_fan_ji_xian_v221121, "{freq}_D{di}_反击线V221121", "JccFanJiXianV221121"};
  m["jcc_fen_shou_xian_V20221113"] = {SignalCategory::kKline, jcc_fen_shou_xian_v20221113, "{freq}_D{di}K_分手线V20221113", "JccFenShouXianV20221113"};
  m["jcc_gap_yin_yang_V221121"] = {SignalCategory::kKline, jcc_gap_yin_yang_v221121, "{freq}_D{di}K_并列阴阳V221121", "JccGapYinYangV221121"};
  m["jcc_ping_tou_V221113"] = {SignalCategory::kKline, jcc_ping_tou_v221113, "{freq}_D{di}TH{th}_平头形态V221113", "JccPingTouV221113"};
  m["jcc_san_fa_V20221115"] = {SignalCategory::kKline, jcc_san_fa_v20221115, "{freq}_D{di}K_三法V20221115", "JccSanFaV20221115"};
  m["jcc_san_fa_V20221118"] = {SignalCategory::kKline, jcc_san_fa_v20221118, "{freq}_D{di}K_三法AV20221118", "JccSanFaV20221118"};
  m["jcc_san_szx_V221122"] = {SignalCategory::kKline, jcc_san_szx_v221122, "{freq}_D{di}T{th}_三星V221122", "JccSanSzxV221122"};
  m["jcc_san_xing_xian_V221023"] = {SignalCategory::kKline, jcc_san_xing_xian_v221023, "{freq}_D{di}TH{th}_伞形线V221023", "JccSanXingXianV221023"};
  m["jcc_shan_chun_V221121"] = {SignalCategory::kKline, jcc_shan_chun_v221121, "{freq}_D{di}B_山川形态V221121", "JccShanChunV221121"};
  m["jcc_szx_V221111"] = {SignalCategory::kKline, jcc_szx_v221111, "{freq}_D{di}TH{th}_十字线V221111", "JccSzxV221111"};
  m["jcc_ta_xing_V221124"] = {SignalCategory::kKline, jcc_ta_xing_v221124, "{freq}_D{di}K_塔形V221124", "JccTaXingV221124"};
  m["jcc_ten_mo_V221028"] = {SignalCategory::kKline, jcc_ten_mo_v221028, "{freq}_D{di}_吞没形态V221028", "JccTenMoV221028"};
  m["jcc_three_crow_V221108"] = {SignalCategory::kKline, jcc_three_crow_v221108, "{freq}_D{di}_三只乌鸦V221108", "JccThreeCrowV221108"};
  m["jcc_two_crow_V221108"] = {SignalCategory::kKline, jcc_two_crow_v221108, "{freq}_D{di}K_两只乌鸦V221108", "JccTwoCrowV221108"};
  m["jcc_wu_yun_gai_ding_V221101"] = {SignalCategory::kKline, jcc_wu_yun_gai_ding_v221101, "{freq}_D{di}Z{z}TH{th}_乌云盖顶V221101", "JccWuYunGaiDingV221101"};
  m["jcc_xing_xian_V221118"] = {SignalCategory::kKline, jcc_xing_xian_v221118, "{freq}_D{di}TH{th}_星形线V221118", "JccXingXianV221118"};
  m["jcc_yun_xian_V221118"] = {SignalCategory::kKline, jcc_yun_xian_v221118, "{freq}_D{di}_孕线V221118", "JccYunXianV221118"};
  m["jcc_zhu_huo_xian_V221027"] = {SignalCategory::kKline, jcc_zhu_huo_xian_v221027, "{freq}_D{di}T{th}F{zf}_烛火线V221027", "JccZhuHuoXianV221027"};
  m["kcatr_up_dw_line_V230823"] = {SignalCategory::kKline, kcatr_up_dw_line_v230823, "{freq}_D{di}N{n}M{m}T{th}_KCATR多空V230823", "KcatrUpDwLineV230823"};
  m["ntmdk_V230824"] = {SignalCategory::kKline, ntmdk_v230824, "{freq}_D{di}M{m}_NTMDK多空V230824", "NtmdkV230824"};
  m["obv_up_dw_line_V230719"] = {SignalCategory::kKline, obv_up_dw_line_v230719, "{freq}_D{di}N{n}M{m}MO{max_overlap}_OBV能量V230719", "ObvUpDwLineV230719"};
  m["obvm_line_V230610"] = {SignalCategory::kKline, obvm_line_v230610, "{freq}_D{di}N{n}M{m}_OBV能量V230610", "ObvmLineV230610"};
  m["pos_bar_stop_V230524"] = {SignalCategory::kTrader, pos_bar_stop_v230524, "{pos_name}_{freq1}N{n}K_止损V230524", "PosBarStopV230524"};
  m["pos_fix_exit_V230624"] = {SignalCategory::kTrader, pos_fix_exit_v230624, "{pos_name}_固定{th}BP止盈止损_出场V230624", "PosFixExitV230624"};
  m["pos_fx_stop_V230414"] = {SignalCategory::kTrader, pos_fx_stop_v230414, "{freq1}_{pos_name}N{n}_止损V230414", "PosFxStopV230414"};
  m["pos_holds_V230414"] = {SignalCategory::kTrader, pos_holds_v230414, "{pos_name}_{freq1}N{n}M{m}_趋势判断V230414", "PosHoldsV230414"};
  m["pos_holds_V230807"] = {SignalCategory::kTrader, pos_holds_v230807, "{pos_name}_{freq1}N{n}M{m}T{t}_BS辅助V230807", "PosHoldsV230807"};
  m["pos_holds_V240428"] = {SignalCategory::kTrader, pos_holds_v240428, "{pos_name}_{freq1}H{h}T{t}N{n}_保本V240428", "PosHoldsV240428"};
  m["pos_holds_V240608"] = {SignalCategory::kTrader, pos_holds_v240608, "{pos_name}_{freq1}W{w}N{n}_保本V240608", "PosHoldsV240608"};
  m["pos_ma_V230414"] = {SignalCategory::kTrader, pos_ma_v230414, "{pos_name}_{freq1}#{ma_type}#{timeperiod}_持有状态V230414", "PosMaV230414"};
  m["pos_profit_loss_V230624"] = {SignalCategory::kTrader, pos_profit_loss_v230624, "{pos_name}_{freq1}YKB{ykb}N{n}_盈亏比判断V230624", "PosProfitLossV230624"};
  m["pos_status_V230808"] = {SignalCategory::kTrader, pos_status_v230808, "{pos_name}_持仓状态_BS辅助V230808", "PosStatusV230808"};
  m["pos_stop_V240331"] = {SignalCategory::kTrader, pos_stop_v240331, "{pos_name}_{freq1}#{n}_止损V240331", "PosStopV240331"};
  m["pos_stop_V240428"] = {SignalCategory::kTrader, pos_stop_v240428, "{pos_name}_{freq1}T{t}N{n}_止损V240428", "PosStopV240428"};
  m["pos_stop_V240608"] = {SignalCategory::kTrader, pos_stop_v240608, "{pos_name}_{freq1}W{w}N{n}_止损V240608", "PosStopV240608"};
  m["pos_stop_V240614"] = {SignalCategory::kTrader, pos_stop_v240614, "{pos_name}_{freq1}N{n}_止损V240614", "PosStopV240614"};
  m["pos_stop_V240717"] = {SignalCategory::kTrader, pos_stop_v240717, "{pos_name}_{freq1}N{n}T{timeperiod}_止损V240717", "PosStopV240717"};
  m["pos_take_V240428"] = {SignalCategory::kTrader, pos_take_v240428, "{pos_name}_{freq1}T{t}N{n}_止盈V240428", "PosTakeV240428"};
  m["pressure_support_V240222"] = {SignalCategory::kKline, pressure_support_v240222, "{freq}_D{di}W{w}高低点验证_支撑压力V240222", "PressureSupportV240222"};
  m["pressure_support_V240402"] = {SignalCategory::kKline, pressure_support_v240402, "{freq}_D{di}W{w}_支撑压力V240402", "PressureSupportV240402"};
  m["pressure_support_V240406"] = {SignalCategory::kKline, pressure_support_v240406, "{freq}_D{di}W{w}_支撑压力V240406", "PressureSupportV240406"};
  m["pressure_support_V240530"] = {SignalCategory::kKline, pressure_support_v240530, "{freq}_D{di}W{w}N{n}_支撑压力V240530", "PressureSupportV240530"};
  m["skdj_up_dw_line_V230611"] = {SignalCategory::kKline, skdj_up_dw_line_v230611, "{freq}_D{di}N{n}M{m}UP{up}DW{dw}_SKDJ随机波动V230611", "SkdjUpDwLineV230611"};
  m["tas_accelerate_V230531"] = {SignalCategory::kKline, tas_accelerate_v230531, "{freq}_D{di}N{n}T{t}_BOLL加速V230531", "TasAccelerateV230531"};
  m["tas_angle_V230802"] = {SignalCategory::kKline, tas_angle_v230802, "{freq}_D{di}N{n}T{th}_笔角度V230802", "TasAngleV230802"};
  m["tas_atr_V230630"] = {SignalCategory::kKline, tas_atr_v230630, "{freq}_D{di}ATR{timeperiod}_波动V230630", "TasAtrV230630"};
  m["tas_atr_break_V230424"] = {SignalCategory::kKline, tas_atr_break_v230424, "{freq}_D{di}ATR{timeperiod}T{th}突破_BS辅助V230424", "TasAtrBreakV230424"};
  m["tas_boll_bc_V221118"] = {SignalCategory::kKline, tas_boll_bc_v221118, "{freq}_D{di}N{n}M{m}L{line}#BOLL{timeperiod}_背驰V221118", "TasBollBcV221118"};
  m["tas_boll_cc_V230312"] = {SignalCategory::kKline, tas_boll_cc_v230312, "{freq}_D{di}BOLL{timeperiod}S{nbdev}SP{sp}_BS辅助V230312", "TasBollCcV230312"};
  m["tas_boll_power_V221112"] = {SignalCategory::kKline, tas_boll_power_v221112, "{freq}_D{di}BOLL{timeperiod}_强弱V221112", "TasBollPowerV221112"};
  m["tas_boll_vt_V230212"] = {SignalCategory::kKline, tas_boll_vt_v230212, "{freq}_D{di}BOLL{timeperiod}S{nbdev}MO{max_overlap}_BS辅助V230212", "TasBollVtV230212"};
  m["tas_cci_base_V230402"] = {SignalCategory::kKline, tas_cci_base_v230402, "{freq}_D{di}CCI{timeperiod}#{min_count}#{max_count}_BS辅助V230402", "TasCciBaseV230402"};
  m["tas_cross_status_V230619"] = {SignalCategory::kKline, tas_cross_status_v230619, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_金死叉V230619", "TasCrossStatusV230619"};
  m["tas_cross_status_V230624"] = {SignalCategory::kKline, tas_cross_status_v230624, "{freq}_D{di}N{n}MD{md}_MACD交叉数量V230624", "TasCrossStatusV230624"};
  m["tas_cross_status_V230625"] = {SignalCategory::kKline, tas_cross_status_v230625, "{freq}_D{di}N{n}MD{md}J{j}S{s}_MACD交叉数量V230625", "TasCrossStatusV230625"};
  m["tas_dif_layer_V241010"] = {SignalCategory::kKline, tas_dif_layer_v241010, "{freq}_DIF分层W{w}T{t}_完全分类V241010", "TasDifLayerV241010"};
  m["tas_dif_zero_V240612"] = {SignalCategory::kKline, tas_dif_zero_v240612, "{freq}_DIF靠近零轴T{t}_BS辅助V240612", "TasDifZeroV240612"};
  m["tas_dif_zero_V240614"] = {SignalCategory::kKline, tas_dif_zero_v240614, "{freq}_DIF靠近零轴W{w}T{t}_BS辅助V240614", "TasDifZeroV240614"};
  m["tas_dma_bs_V240608"] = {SignalCategory::kKline, tas_dma_bs_v240608, "{freq}_N{n}双均线{t1}#{t2}顺势_BS辅助V240608", "TasDmaBsV240608"};
  m["tas_double_ma_V221203"] = {SignalCategory::kKline, tas_double_ma_v221203, "{freq}_D{di}T{th}#{ma_type}#{timeperiod1}#{timeperiod2}_JX辅助V221203", "TasDoubleMaV221203"};
  m["tas_double_ma_V230511"] = {SignalCategory::kKline, tas_double_ma_v230511, "{freq}_D{di}#{ma_type}#{t1}#{t2}_BS辅助V230511", "TasDoubleMaV230511"};
  m["tas_double_ma_V240208"] = {SignalCategory::kKline, tas_double_ma_v240208, "{freq}_D{di}N{N}M{M}双均线_BS辅助V240208", "TasDoubleMaV240208"};
  m["tas_first_bs_V230217"] = {SignalCategory::kKline, tas_first_bs_v230217, "{freq}_D{di}N{n}#{ma_type}#{timeperiod}_BS1辅助V230217", "TasFirstBsV230217"};
  m["tas_hlma_V230301"] = {SignalCategory::kKline, tas_hlma_v230301, "{freq}_D{di}#{ma_type}#{timeperiod}HLMA_BS辅助V230301", "TasHlmaV230301"};
  m["tas_kdj_base_V221101"] = {SignalCategory::kKline, tas_kdj_base_v221101, "{freq}_D{di}K#KDJ{fastk_period}#{slowk_period}#{slowd_period}_KDJ辅助V221101", "TasKdjBaseV221101"};
  m["tas_kdj_evc_V221201"] = {SignalCategory::kKline, tas_kdj_evc_v221201, "{freq}_D{di}T{th}KDJ{fastk_period}#{slowk_period}#{slowd_period}#{key}值突破{c1}#{c2}_KDJ极值V221201", "TasKdjEvcV221201"};
  m["tas_kdj_evc_V230401"] = {SignalCategory::kKline, tas_kdj_evc_v230401, "{freq}_D{di}T{th}KDJ{fastk_period}#{slowk_period}#{slowd_period}#{key}值突破{min_count}#{max_count}_BS辅助V230401", "TasKdjEvcV230401"};
  m["tas_low_trend_V230627"] = {SignalCategory::kKline, tas_low_trend_v230627, "{freq}_D{di}N{n}TH{th}_趋势230627", "TasLowTrendV230627"};
  m["tas_ma_base_V221101"] = {SignalCategory::kKline, tas_ma_base_v221101, "{freq}_D{di}{ma_type}#{timeperiod}_分类V221101", "TasMaBaseV221101"};
  m["tas_ma_base_V221203"] = {SignalCategory::kKline, tas_ma_base_v221203, "{freq}_D{di}{ma_type}#{timeperiod}T{th}_分类V221203", "TasMaBaseV221203"};
  m["tas_ma_base_V230313"] = {SignalCategory::kKline, tas_ma_base_v230313, "{freq}_D{di}#{ma_type}#{timeperiod}MO{max_overlap}_BS辅助V230313", "TasMaBaseV230313"};
  m["tas_ma_cohere_V230512"] = {SignalCategory::kKline, tas_ma_cohere_v230512, "{freq}_D{di}SMA{ma_seq}_均线系统V230512", "TasMaCohereV230512"};
  m["tas_ma_round_V221206"] = {SignalCategory::kKline, tas_ma_round_v221206, "{freq}_D{di}TH{th}#碰{ma_type}#{timeperiod}_BE辅助V221206", "TasMaRoundV221206"};
  m["tas_ma_system_V230513"] = {SignalCategory::kKline, tas_ma_system_v230513, "{freq}_D{di}SMA{ma_seq}_均线系统V230513", "TasMaSystemV230513"};
  m["tas_macd_base_V221028"] = {SignalCategory::kKline, tas_macd_base_v221028, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_BS辅助V221028", "TasMacdBaseV221028"};
  m["tas_macd_base_V230320"] = {SignalCategory::kKline, tas_macd_base_v230320, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}MO{max_overlap}#{key}_BS辅助V230320", "TasMacdBaseV230320"};
  m["tas_macd_bc_V221201"] = {SignalCategory::kKline, tas_macd_bc_v221201, "{freq}_D{di}N{n}M{m}#MACD{fastperiod}#{slowperiod}#{signalperiod}_BCV221201", "TasMacdBcV221201"};
  m["tas_macd_bc_V230803"] = {SignalCategory::kKline, tas_macd_bc_v230803, "{freq}_MACD双分型背驰_BS辅助V230803", "TasMacdBcV230803"};
  m["tas_macd_bc_V230804"] = {SignalCategory::kKline, tas_macd_bc_v230804, "{freq}_D{di}MACD背驰_BS辅助V230804", "TasMacdBcV230804"};
  m["tas_macd_bc_V240307"] = {SignalCategory::kKline, tas_macd_bc_v240307, "{freq}_D{di}N{n}柱子背驰_BS辅助V240307", "TasMacdBcV240307"};
  m["tas_macd_bc_ubi_V230804"] = {SignalCategory::kKline, tas_macd_bc_ubi_v230804, "{freq}_MACD背驰_UBI观察V230804", "TasMacdBcUbiV230804"};
  m["tas_macd_bs1_V230312"] = {SignalCategory::kKline, tas_macd_bs1_v230312, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_BS1辅助V230312", "TasMacdBs1V230312"};
  m["tas_macd_bs1_V230313"] = {SignalCategory::kKline, tas_macd_bs1_v230313, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_BS1辅助V230313", "TasMacdBs1V230313"};
  m["tas_macd_bs1_V230411"] = {SignalCategory::kKline, tas_macd_bs1_v230411, "{freq}_D{di}T{tha}#{thb}#{thc}_BS1辅助V230411", "TasMacdBs1V230411"};
  m["tas_macd_bs1_V230412"] = {SignalCategory::kKline, tas_macd_bs1_v230412, "{freq}_D{di}T{tha}#{thb}_BS1辅助V230412", "TasMacdBs1V230412"};
  m["tas_macd_change_V221105"] = {SignalCategory::kKline, tas_macd_change_v221105, "{freq}_D{di}K{n}#MACD{fastperiod}#{slowperiod}#{signalperiod}变色次数_BS辅助V221105", "TasMacdChangeV221105"};
  m["tas_macd_direct_V221106"] = {SignalCategory::kKline, tas_macd_direct_v221106, "{freq}_D{di}K#MACD{fastperiod}#{slowperiod}#{signalperiod}方向_BS辅助V221106", "TasMacdDirectV221106"};
  m["tas_macd_dist_V230408"] = {SignalCategory::kKline, tas_macd_dist_v230408, "{freq}_{key}分层W{w}N{n}_BS辅助V230408", "TasMacdDistV230408"};
  m["tas_macd_dist_V230409"] = {SignalCategory::kKline, tas_macd_dist_v230409, "{freq}_{key}远离W{w}N{n}T{t}_BS辅助V230409", "TasMacdDistV230409"};
  m["tas_macd_dist_V230410"] = {SignalCategory::kKline, tas_macd_dist_v230410, "{freq}_{key}多空分层W{w}N{n}_BS辅助V230410", "TasMacdDistV230410"};
  m["tas_macd_first_bs_V221201"] = {SignalCategory::kKline, tas_macd_first_bs_v221201, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_BS1辅助V221201", "TasMacdFirstBsV221201"};
  m["tas_macd_first_bs_V221216"] = {SignalCategory::kKline, tas_macd_first_bs_v221216, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_BS1辅助V221216", "TasMacdFirstBsV221216"};
  m["tas_macd_power_V221108"] = {SignalCategory::kKline, tas_macd_power_v221108, "{freq}_D{di}K#MACD{fastperiod}#{slowperiod}#{signalperiod}强弱_BS辅助V221108", "TasMacdPowerV221108"};
  m["tas_macd_second_bs_V221201"] = {SignalCategory::kKline, tas_macd_second_bs_v221201, "{freq}_D{di}MACD{fastperiod}#{slowperiod}#{signalperiod}_BS2辅助V221201", "TasMacdSecondBsV221201"};
  m["tas_macd_xt_V221208"] = {SignalCategory::kKline, tas_macd_xt_v221208, "{freq}_D{di}K#MACD{fastperiod}#{slowperiod}#{signalperiod}形态_BS辅助V221208", "TasMacdXtV221208"};
  m["tas_rsi_base_V230227"] = {SignalCategory::kKline, tas_rsi_base_v230227, "{freq}_D{di}T{th}RSI{timeperiod}_RSI辅助V230227", "TasRsiBaseV230227"};
  m["tas_rumi_V230704"] = {SignalCategory::kKline, tas_rumi_v230704, "{freq}_D{di}F{timeperiod1}S{timeperiod2}R{rumi_window}_BS辅助V230704", "TasRumiV230704"};
  m["tas_sar_base_V230425"] = {SignalCategory::kKline, tas_sar_base_v230425, "{freq}_D{di}MO{max_overlap}SAR_BS辅助V230425", "TasSarBaseV230425"};
  m["tas_second_bs_V230228"] = {SignalCategory::kKline, tas_second_bs_v230228, "{freq}_D{di}N{n}#{ma_type}#{timeperiod}_BS2辅助V230228", "TasSecondBsV230228"};
  m["tas_second_bs_V230303"] = {SignalCategory::kKline, tas_second_bs_v230303, "{freq}_D{di}{ma_type}#{timeperiod}_BS2辅助V230303", "TasSecondBsV230303"};
  m["tas_slope_V231019"] = {SignalCategory::kKline, tas_slope_v231019, "{freq}_D{di}DIF{n}斜率T{th}_BS辅助V231019", "TasSlopeV231019"};
  m["vol_double_ma_V230214"] = {SignalCategory::kKline, vol_double_ma_v230214, "{freq}_D{di}VOL双均线{ma_type}#{t1}#{t2}_BS辅助V230214", "VolDoubleMaV230214"};
  m["vol_gao_di_V221218"] = {SignalCategory::kKline, vol_gao_di_v221218, "{freq}_D{di}K_量柱V221218", "VolGaoDiV221218"};
  m["vol_single_ma_V230214"] = {SignalCategory::kKline, vol_single_ma_v230214, "{freq}_D{di}VOL#{ma_type}#{timeperiod}_分类V230214", "VolSingleMaV230214"};
  m["vol_ti_suo_V221216"] = {SignalCategory::kKline, vol_ti_suo_v221216, "{freq}_D{di}K_量柱V221216", "VolTiSuoV221216"};
  m["vol_window_V230731"] = {SignalCategory::kKline, vol_window_v230731, "{freq}_D{di}W{w}M{m}N{n}_窗口能量V230731", "VolWindowV230731"};
  m["vol_window_V230801"] = {SignalCategory::kKline, vol_window_v230801, "{freq}_D{di}W{w}_窗口能量V230801", "VolWindowV230801"};
  m["xl_bar_basis_V240411"] = {SignalCategory::kKline, xl_bar_basis_v240411, "{freq}_N{n}_形态V240411", "XlBarBasisV240411"};
  m["xl_bar_basis_V240412"] = {SignalCategory::kKline, xl_bar_basis_v240412, "{freq}_N{n}#TH{th}_形态V240412", "XlBarBasisV240412"};
  m["xl_bar_position_V240328"] = {SignalCategory::kKline, xl_bar_position_v240328, "{freq}_N{n}_BS辅助V240328", "XlBarPositionV240328"};
  m["xl_bar_trend_V240329"] = {SignalCategory::kKline, xl_bar_trend_v240329, "{freq}_N{n}M{m}_十字线反转V240329", "XlBarTrendV240329"};
  m["xl_bar_trend_V240330"] = {SignalCategory::kKline, xl_bar_trend_v240330, "{freq}_N{n}M{m}#{ma_type}_双均线过滤V240330", "XlBarTrendV240330"};
  m["xl_bar_trend_V240331"] = {SignalCategory::kKline, xl_bar_trend_v240331, "{freq}_N{n}_突破信号V240331", "XlBarTrendV240331"};
  m["xl_bar_trend_V240623"] = {SignalCategory::kKline, xl_bar_trend_v240623, "{freq}_N{n}通道_突破信号V240623", "XlBarTrendV240623"};
  m["zdy_bi_end_V230406"] = {SignalCategory::kKline, zdy_bi_end_v230406, "{freq}_D0停顿分型_BE辅助V230406", "ZdyBiEndV230406"};
  m["zdy_bi_end_V230407"] = {SignalCategory::kKline, zdy_bi_end_v230407, "{freq}_D0停顿分型_BE辅助V230407", "ZdyBiEndV230407"};
  m["zdy_dif_V230527"] = {SignalCategory::kKline, zdy_dif_v230527, "{freq}_N{n}T{t}_DIF远离V230527", "ZdyDifV230527"};
  m["zdy_dif_V230528"] = {SignalCategory::kKline, zdy_dif_v230528, "{freq}_N{n}T{t}_DIF远离V230528", "ZdyDifV230528"};
  m["zdy_macd_V230518"] = {SignalCategory::kKline, zdy_macd_v230518, "{freq}_D{di}MACD交叉N{n}_BS辅助V230518", "ZdyMacdV230518"};
  m["zdy_macd_V230519"] = {SignalCategory::kKline, zdy_macd_v230519, "{freq}_D{di}N{n}MACD缩柱_BS辅助V230519", "ZdyMacdV230519"};
  m["zdy_macd_V230527"] = {SignalCategory::kKline, zdy_macd_v230527, "{freq}_{key}远离W{w}N{n}T{t}_BS辅助V230527", "ZdyMacdV230527"};
  m["zdy_macd_bc_V230422"] = {SignalCategory::kKline, zdy_macd_bc_v230422, "{freq}_D{di}T{th}MACD面积背驰_BS辅助V230422", "ZdyMacdBcV230422"};
  m["zdy_macd_bs1_V230422"] = {SignalCategory::kKline, zdy_macd_bs1_v230422, "{freq}_D{di}T{th}MACD_BS1辅助V230422", "ZdyMacdBs1V230422"};
  m["zdy_macd_dif_V230516"] = {SignalCategory::kKline, zdy_macd_dif_v230516, "{freq}_D{di}DIF走平_BS辅助V230516", "ZdyMacdDifV230516"};
  m["zdy_macd_dif_V230517"] = {SignalCategory::kKline, zdy_macd_dif_v230517, "{freq}_D{di}MACD开仓_BS辅助V230517", "ZdyMacdDifV230517"};
  m["zdy_macd_dif_iqr_V230521"] = {SignalCategory::kKline, zdy_macd_dif_iqr_v230521, "{freq}_D{di}DIF走平IQR_BS辅助V230521", "ZdyMacdDifIqrV230521"};
  m["zdy_stop_loss_V230406"] = {SignalCategory::kTrader, zdy_stop_loss_v230406, "{freq1}_{pos_name}F{first_stop}_止损V230406", "ZdyStopLossV230406"};
  m["zdy_take_profit_V230406"] = {SignalCategory::kTrader, zdy_take_profit_v230406, "{freq1}_{pos_name}_止盈V230406", "ZdyTakeProfitV230406"};
  m["zdy_take_profit_V230407"] = {SignalCategory::kTrader, zdy_take_profit_v230407, "{freq1}_{pos_name}_止盈V230407", "ZdyTakeProfitV230407"};
  m["zdy_vibrate_V230406"] = {SignalCategory::kTrader, zdy_vibrate_v230406, "中枢震荡_{freq1}#{freq2}_BS辅助V230406", "ZdyVibrateV230406"};
  m["zdy_zs_V230423"] = {SignalCategory::kKline, zdy_zs_v230423, "{freq}_D{di}中枢形态_BS辅助V230423", "ZdyZsV230423"};
  m["zdy_zs_space_V230421"] = {SignalCategory::kKline, zdy_zs_space_v230421, "{freq}_D{di}中枢空间_BS辅助V230421", "ZdyZsSpaceV230421"};

  return m;
}

const std::unordered_map<std::string, SignalMeta>& signal_registry() {
  static auto registry = build_registry();
  return registry;
}

std::vector<RegisteredSignalInfo> list_all_signals() {
  std::vector<RegisteredSignalInfo> out;
  for (auto& [name, meta] : signal_registry()) {
    std::string cat = (meta.category == SignalCategory::kKline) ? "kline" : "trader";
    std::string ns = name.substr(0, name.find('_'));
    out.push_back({name, std::string(meta.param_template), cat, ns});
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
    if (a.category != b.category) return a.category < b.category;
    return a.name < b.name;
  });
  return out;
}

std::vector<Signal> run_signal(const char* name, const CZSC& czsc,
                               const ParamView& params, TaCache* cache) {
  auto& reg = signal_registry();
  auto it = reg.find(name);
  if (it == reg.end()) return {};
  return it->second.func(czsc, params, cache);
}

}  // namespace czsc::signals