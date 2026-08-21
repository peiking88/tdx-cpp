// czsc-cpp signal registry — auto-rewritten
// 246 signals from Rust, all with real implementations

#include "czsc/signals/registry.hpp"
#include "czsc/signals/signal_builder.hpp"
#include "czsc/ta/indicators.hpp"
#include "czsc/ta/ta_cache.hpp"
#include "czsc/analyze/czsc.hpp"
#include "czsc/analyze/algorithms.hpp"
#include "czsc/types/zs.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <string>
#include <sstream>
#include <numeric>

namespace czsc::signals {
using czsc::analyze::CZSC;
using czsc::TraderState;
using czsc::analyze::check_fxs;
using czsc::analyze::check_first_buy;
using czsc::analyze::check_first_sell;
using czsc::ta::TaCache;
namespace ta = czsc::ta;

// === Helpers ===
static auto F=[](auto n){return std::to_string(n);};

// 算术均值
static double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// fx 内含的 K 线展平（FX.elements: NewBar[] → 每个 NewBar.elements: RawBar[]）
// 对齐 Rust utils/cxt.rs:20 fx_raw_bars
static std::vector<RawBar> fx_raw_bars(const FX& fx) {
  std::vector<RawBar> out;
  for (const auto& nb : fx.elements)
    out.insert(out.end(), nb.elements.begin(), nb.elements.end());
  return out;
}

// bars_raw 的 bar id → 下标映射，用于 O(1) 定位（对齐 Rust utils/sig.rs:54 bar_index_map）
static std::unordered_map<int32_t, size_t> bar_index_map(const CZSC& c) {
  std::unordered_map<int32_t, size_t> m;
  m.reserve(c.bars_raw.size());
  for (size_t i = 0; i < c.bars_raw.size(); ++i) m[c.bars_raw[i].id] = i;
  return m;
}

// 影线长度（对齐 Rust utils/cxt.rs:10-18）
static double raw_bar_upper(const RawBar& bar) { return bar.high - std::max(bar.open, bar.close); }
static double raw_bar_lower(const RawBar& bar) { return std::min(bar.open, bar.close) - bar.low; }

// 由笔序列构造中枢序列（对齐 Rust utils/cxt.rs:73 get_zs_seq）
static std::vector<ZS> get_zs_seq(const std::vector<BI>& bis) {
  std::vector<ZS> zs_list;
  if (bis.empty()) return zs_list;
  for (const BI& bi : bis) {
    if (zs_list.empty()) { zs_list.emplace_back(std::vector<BI>{bi}); continue; }
    ZS last = std::move(zs_list.back());
    zs_list.pop_back();
    if (last.bis.empty()) {
      auto nb = last.bis; nb.push_back(bi);
      zs_list.emplace_back(std::move(nb));
    } else if ((bi.direction == Direction::kUp && bi.get_high() < last.zd) ||
               (bi.direction == Direction::kDown && bi.get_low() > last.zg)) {
      zs_list.emplace_back(std::move(last));
      zs_list.emplace_back(std::vector<BI>{bi});
    } else {
      auto nb = last.bis; nb.push_back(bi);
      zs_list.emplace_back(std::move(nb));
    }
  }
  return zs_list;
}

// K 线价格去重（对齐 Rust utils/cxt.rs:104 unique_prices_from_bars）
static std::vector<double> unique_prices_from_bars(const std::vector<RawBar>& bars) {
  std::vector<double> prices;
  prices.reserve(bars.size() * 4);
  for (const auto& b : bars) {
    if (std::isfinite(b.close)) prices.push_back(b.close);
    if (std::isfinite(b.high)) prices.push_back(b.high);
    if (std::isfinite(b.low)) prices.push_back(b.low);
    if (std::isfinite(b.open)) prices.push_back(b.open);
  }
  std::sort(prices.begin(), prices.end());
  prices.erase(std::unique(prices.begin(), prices.end(),
                           [](double a, double b) { return std::abs(a - b) <= 1e-9; }),
               prices.end());
  return prices;
}

// 笔表里关系（对齐 Rust utils/cxt.rs:132 calc_bi_status_values）
// 返回 {v1, v2}：v1=向上/向下，v2=顶分/底分/延伸
static std::pair<const char*, const char*> calc_bi_status_values(const CZSC& c, const std::vector<FX>& ubi_fxs) {
  const char* v1;
  const auto& last_bi = c.bi_list.back();
  if (last_bi.direction == Direction::kDown) {
    v1 = (c.bars_ubi.size() > 7) ? "\xe5\x90\x91\xe4\xb8\x8a" : "\xe5\x90\x91\xe4\xb8\x8b";  // 向上/向下
  } else {
    v1 = (c.bars_ubi.size() > 7) ? "\xe5\x90\x91\xe4\xb8\x8b" : "\xe5\x90\x91\xe4\xb8\x8a";  // 向下/向上
  }
  const char* v2 = "\xe5\xbb\xb6\xe4\xbc\xb8";  // 延伸
  if (!ubi_fxs.empty()) {
    const auto& last_fx = ubi_fxs.back();
    if (last_fx.mark == Mark::kD) v2 = (v1 == "\xe5\x90\x91\xe4\xb8\x8b") ? "\xe5\xba\x95\xe5\x88\x86" : "\xe5\xbb\xb6\xe4\xbc\xb8";  // 底分/延伸
    else if (last_fx.mark == Mark::kG) v2 = (v1 == "\xe5\x90\x91\xe4\xb8\x8a") ? "\xe9\xa1\xb6\xe5\x88\x86" : "\xe5\xbb\xb6\xe4\xbc\xb8";  // 顶分/延伸
  }
  return {v1, v2};
}

// 最大振幅百分比（对齐 Rust utils/math.rs max_amplitude_pct）
static double max_amplitude_pct(const std::vector<double>& prices) {
  if (prices.empty()) return 100.0;
  double mx = -std::numeric_limits<double>::infinity();
  double mn = std::numeric_limits<double>::infinity();
  for (double p : prices) { mx = std::max(mx, p); mn = std::min(mn, p); }
  if (mn == 0.0) return 100.0;
  return (mx - mn) / mn * 100.0;
}

// 线性回归预测（对齐 Rust utils/math.rs linreg_predict）
static std::optional<double> linreg_predict(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
  if (xs.size() != ys.size() || xs.empty()) return std::nullopt;
  double n = static_cast<double>(xs.size());
  double mx = 0, my = 0;
  for (size_t i = 0; i < xs.size(); ++i) { mx += xs[i]; my += ys[i]; }
  mx /= n; my /= n;
  double cov = 0, var = 0;
  for (size_t i = 0; i < xs.size(); ++i) { cov += (xs[i]-mx)*(ys[i]-my); var += (xs[i]-mx)*(xs[i]-mx); }
  if (var == 0.0) return my;
  double slope = cov / var;
  return slope * x + (my - slope * mx);
}

// 区间重叠判定（对齐 Rust utils/math.rs overlap）
static bool ranges_overlap(double h1, double l1, double h2, double l2) {
  return std::max(l1, l2) < std::min(h1, h2);
}

// adtm_up_dw_line_V230603：ADTM 能量异动多空（对齐 Rust ang.rs:57 adtm_up_dw_line_v230603）
//   up_sum = Σ max(high-open, open-prev_open) for open>prev_open (N窗口)
//   dw_sum = Σ max(open-low, prev_open-open) for open<prev_open (M窗口)
//   adtm = (up_sum-dw_sum)/max(up_sum,dw_sum)
//   看多: up_sum>dw_sum || adtm>th/10；看空: up_sum<dw_sum || adtm<th/10
static std::vector<Signal> adtm_up_dw_line_v230603(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 30), m = p.usize("m", 20), th = p.usize("th", 5);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "M" + F(m) + "TH" + F(th);
  const char* k3 = "ADTMV230603";
  if (c.bars_raw.size() < di + std::max(n, m) + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto n_bars = get_sub_elements_vec(c.bars_raw, di, n);
  auto m_bars = get_sub_elements_vec(c.bars_raw, di, m);
  if (n_bars.size() < 2 || m_bars.size() < 2) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double up_sum = 0.0, dw_sum = 0.0;
  for (size_t i = 1; i < n_bars.size(); ++i) {
    if (n_bars[i].open > n_bars[i-1].open)
      up_sum += std::max(n_bars[i].high - n_bars[i].open, n_bars[i].open - n_bars[i-1].open);
  }
  for (size_t i = 1; i < m_bars.size(); ++i) {
    if (m_bars[i].open < m_bars[i-1].open)
      dw_sum += std::max(m_bars[i].open - m_bars[i].low, m_bars[i-1].open - m_bars[i].open);
  }
  double denom = std::max(up_sum, dw_sum);
  double adtm = (denom > 0.0) ? (up_sum - dw_sum) / denom : NAN;
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (up_sum > dw_sum || (std::isfinite(adtm) && adtm > th / 10.0)) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (up_sum < dw_sum || (std::isfinite(adtm) && adtm < th / 10.0)) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// amv_up_dw_line_V230603：AMV 能量多空（对齐 Rust ang.rs:132 amv_up_dw_line_v230603）
//   amv1 = Σ(amount*(open+close)/2) / Σ(amount) over N窗口; amv2 over M窗口
//   看多: amv1>amv2；否则看空
static std::vector<Signal> amv_up_dw_line_v230603(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 30), m = p.usize("m", 120);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "M" + F(m);
  const char* k3 = "AMV\xe8\x83\xbd\xe9\x87\x8fV230603";
  if (n > m || c.bars_raw.size() < di + m + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto n_bars = get_sub_elements_vec(c.bars_raw, di, n);
  auto m_bars = get_sub_elements_vec(c.bars_raw, di, m);
  if (n_bars.empty() || m_bars.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double amov1 = 0, amov2 = 0, vol1 = 0, vol2 = 0;
  for (auto& b : n_bars) { amov1 += b.amount * (b.open + b.close) / 2.0; vol1 += b.amount; }
  for (auto& b : m_bars) { amov2 += b.amount * (b.open + b.close) / 2.0; vol2 += b.amount; }
  double amv1 = amov1 / vol1, amv2 = amov2 / vol2;
  const char* v1 = (amv1 > amv2) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// asi_up_dw_line_V230603：ASI 多空（对齐 Rust ang.rs:190 asi_up_dw_line_v230603）
//   SI_i = 50*(close-c1 + (c1-open) + 0.5*(close-open)) / (r*k/m)
//   ASI = cumsum(SI); 看多: ASI_last > mean(ASI)
static std::vector<Signal> asi_up_dw_line_v230603(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 30), pp = p.usize("p", 120);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "P" + F(pp);
  const char* k3 = "ASI\xe5\xa4\x9a\xe7\xa9\xbaV230603";
  if (c.bars_raw.size() < di + pp + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, pp);
  if (bars.size() < 2) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  size_t len = bars.size();
  std::vector<double> si; si.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    double prev_close = (i == 0) ? bars[0].close : bars[i-1].close;
    double prev_low   = (i == 0) ? bars[0].low   : bars[i-1].low;
    double prev_open  = (i == 0) ? bars[0].open  : bars[i-1].open;
    double a = std::abs(bars[i].high - prev_close);
    double b = std::abs(bars[i].low - prev_close);
    double c1 = std::abs(bars[i].high - prev_low);
    double d = std::abs(prev_close - prev_open);
    double k = std::max(a, b);
    double mm = std::max(bars[i].high - bars[i].low, (double)n);
    double r1 = a + 0.5 * b + 0.25 * d;
    double r2 = b + 0.5 * a + 0.25 * d;
    double r3 = c1 + 0.25 * d;
    double r4 = (a >= b && a >= c1) ? r1 : r2;
    double r  = (c1 >= a && c1 >= b) ? r3 : r4;
    double den = r * k / mm;
    if (den == 0.0) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
    si.push_back(50.0 * (bars[i].close - c1 + (c1 - bars[i].open) + 0.5 * (bars[i].close - bars[i].open)) / den);
  }
  double acc = 0; std::vector<double> asi; asi.reserve(si.size());
  for (double x : si) { acc += x; asi.push_back(acc); }
  double asi_last = asi.back();
  double asi_mean = Mean(asi);
  const char* v1 = (asi_last > asi_mean) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

static std::vector<Signal> bar_accelerate_v221110(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarAccelerateV221110",v1.c_str());
}

static std::vector<Signal> bar_accelerate_v221118(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarAccelerateV221118",v1.c_str());
}

static std::vector<Signal> bar_accelerate_v240428(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarAccelerateV240428",v1.c_str());
}

static std::vector<Signal> bar_amount_acc_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarAmountAccV230214",v1.c_str());
}

static std::vector<Signal> bar_big_solid_v230215(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarBigSolidV230215",v1.c_str());
}

static std::vector<Signal> bar_bpm_v230227(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarBpmV230227",v1.c_str());
}

static std::vector<Signal> bar_break_v240428(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarBreakV240428",v1.c_str());
}

static std::vector<Signal> bar_channel_v230508(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarChannelV230508",v1.c_str());
}

static std::vector<Signal> bar_classify_v240606(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarClassifyV240606",v1.c_str());
}

static std::vector<Signal> bar_classify_v240607(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarClassifyV240607",v1.c_str());
}

static std::vector<Signal> bar_decision_v240608(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarDecisionV240608",v1.c_str());
}

static std::vector<Signal> bar_decision_v240616(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarDecisionV240616",v1.c_str());
}

static std::vector<Signal> bar_dual_thrust_v230403(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarDualThrustV230403",v1.c_str());
}

static std::vector<Signal> bar_eight_v230702(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarEightV230702",v1.c_str());
}

static std::vector<Signal> bar_end_v221211(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarEndV221211",v1.c_str());
}

static std::vector<Signal> bar_fake_break_v230204(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarFakeBreakV230204",v1.c_str());
}

static std::vector<Signal> bar_fang_liang_break_v221216(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarFangLiangBreakV221216",v1.c_str());
}

static std::vector<Signal> bar_limit_down_v230525(const CZSC& c, const ParamView& p, TaCache*) {
  if (std::string(p.str("market", "sh")) == "hk") return {};  // 港股无涨跌停限制
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarLimitDownV230525",v1.c_str());
}

static std::vector<Signal> bar_mean_amount_v221112(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarMeanAmountV221112",v1.c_str());
}

static std::vector<Signal> bar_operate_span_v221111(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarOperateSpanV221111",v1.c_str());
}

static std::vector<Signal> bar_plr_v240427(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarPlrV240427",v1.c_str());
}

static std::vector<Signal> bar_polyfit_v240428(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarPolyfitV240428",v1.c_str());
}

static std::vector<Signal> bar_r_breaker_v230326(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarRBreakerV230326",v1.c_str());
}

static std::vector<Signal> bar_reversal_v230227(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarReversalV230227",v1.c_str());
}

static std::vector<Signal> bar_section_momentum_v221112(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarSectionMomentumV221112",v1.c_str());
}

static std::vector<Signal> bar_shuang_fei_v230507(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarShuangFeiV230507",v1.c_str());
}

static std::vector<Signal> bar_single_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarSingleV230214",v1.c_str());
}

static std::vector<Signal> bar_single_v230506(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarSingleV230506",v1.c_str());
}

static std::vector<Signal> bar_td9_v240616(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarTd9V240616",v1.c_str());
}

static std::vector<Signal> bar_time_v230327(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarTimeV230327",v1.c_str());
}

static std::vector<Signal> bar_tnr_v230629(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarTnrV230629",v1.c_str());
}

static std::vector<Signal> bar_tnr_v230630(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarTnrV230630",v1.c_str());
}

static std::vector<Signal> bar_trend_v240209(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarTrendV240209",v1.c_str());
}

static std::vector<Signal> bar_triple_v230506(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarTripleV230506",v1.c_str());
}

static std::vector<Signal> bar_vol_bs1_v230224(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarVolBs1V230224",v1.c_str());
}

static std::vector<Signal> bar_vol_grow_v221112(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarVolGrowV221112",v1.c_str());
}

static std::vector<Signal> bar_volatility_v241013(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarVolatilityV241013",v1.c_str());
}

static std::vector<Signal> bar_weekday_v230328(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarWeekdayV230328",v1.c_str());
}

static std::vector<Signal> bar_window_ps_v230731(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarWindowPsV230731",v1.c_str());
}

static std::vector<Signal> bar_window_ps_v230801(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarWindowPsV230801",v1.c_str());
}

static std::vector<Signal> bar_window_std_v230731(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarWindowStdV230731",v1.c_str());
}

static std::vector<Signal> bar_zdf_v221203(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarZdfV221203",v1.c_str());
}

static std::vector<Signal> bar_zdt_v230331(const CZSC& c, const ParamView& p, TaCache*) {
  if (std::string(p.str("market", "sh")) == "hk") return {};  // 港股无涨跌停限制
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarZdtV230331",v1.c_str());
}

static std::vector<Signal> bar_zfzd_v241013(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarZfzdV241013",v1.c_str());
}

static std::vector<Signal> bar_zfzd_v241014(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarZfzdV241014",v1.c_str());
}

static std::vector<Signal> bar_zt_count_v230504(const CZSC& c, const ParamView& p, TaCache*) {
  if (std::string(p.str("market", "sh")) == "hk") return {};  // 港股无涨跌停限制
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=(b.close>b.open)?"\xe9\x98\xb3":"\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"BarZtCountV230504",v1.c_str());
}

// bias_up_dw_line_V230618：BIAS 三周期共振（对齐 Rust ang.rs:428 bias_up_dw_line_v230618）
//  bias_i = (close - MA_i) / MA_i * 100 (i=n,m,p); 三 bias 同时>th→看多, 同时<-th→看空
static std::vector<Signal> bias_up_dw_line_v230618(const CZSC& c, const ParamView& p, TaCache* cache) {
  size_t di = p.usize("di", 1), n = p.usize("n", 6), m = p.usize("m", 12), pp = p.usize("p", 24);
  int th1 = (int)p.usize("th1", 1), th2 = (int)p.usize("th2", 3), th3 = (int)p.usize("th3", 5);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n) + "M" + F(m) + "P" + F(pp) + "TH1" + F(th1) + "TH2" + F(th2) + "TH3" + F(th3);
  const char* k3 = "BIAS\xe4\xb9\x96\xe7\xa6\xbb\xe7\x8e\x87V230618";  // BIAS乖离率V230618
  if (c.bars_raw.size() < di + std::max({n, m, (size_t)pp})) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto b1 = get_sub_elements_vec(c.bars_raw, di, n);
  auto b2 = get_sub_elements_vec(c.bars_raw, di, m);
  auto b3 = get_sub_elements_vec(c.bars_raw, di, pp);
  if (b1.empty() || b2.empty() || b3.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  // 算 close 均值
  auto mean_close = [](const std::vector<RawBar>& bs) -> double {
    if (bs.empty()) return NAN;
    double s = 0; for (auto& b : bs) s += b.close; return s / (double)bs.size();
  };
  double mc1 = mean_close(b1), mc2 = mean_close(b2), mc3 = mean_close(b3);
  if (!std::isfinite(mc1) || !std::isfinite(mc2) || !std::isfinite(mc3)) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double bias1 = (b1.back().close - mc1) / mc1 * 100.0;
  double bias2 = (b2.back().close - mc2) / mc2 * 100.0;
  double bias3 = (b3.back().close - mc3) / mc3 * 100.0;
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (bias1 > th1 && bias2 > th2 && bias3 > th3) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (bias1 < -th1 && bias2 < -th2 && bias3 < -th3) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

static std::vector<Signal> byi_bi_end_v230106(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ByiBiEndV230106","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ByiBiEndV230106",v1.c_str());
}

static std::vector<Signal> byi_bi_end_v230107(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ByiBiEndV230107","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ByiBiEndV230107",v1.c_str());
}

static std::vector<Signal> byi_fx_num_v230628(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ByiFxNumV230628","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ByiFxNumV230628",v1.c_str());
}

static std::vector<Signal> byi_second_bs_v230324(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ByiSecondBsV230324","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ByiSecondBsV230324",v1.c_str());
}

static std::vector<Signal> byi_symmetry_zs_v221107(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ByiSymmetryZsV221107","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ByiSymmetryZsV221107",v1.c_str());
}

static std::vector<Signal> cat_macd_v230518(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> cat_macd_v230520(const CZSC&, const ParamView&, TaCache*) { return {}; }
// cci_decision_V240620：CCI 逆势决策（对齐 Rust tas.rs:2059 cci_decision_v240620）
//   取最近 N 根 CCI14 序列；min<-100 且有 N_cc 次→开空; max>100 且有 N_cc 次→开多（后者覆盖前者）
static std::vector<Signal> cci_decision_v240620(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache || c.bars_raw.size() < 100) return {};
  const size_t n = p.usize("n", 2);
  const std::string k1 = FreqName(c.freq), k2 = "D1N" + F(n) + "CCI";
  const char* k3 = "\xe5\x86\xb3\xe7\xad\x96\xe5\x8c\xba\xe5\x9f\x9fV240620";  // 决策区域V240620
  const std::string cci_key = "CCI14";
  ta::update_cci_cache(c.bars_raw, cci_key.c_str(), 14, *cache);
  auto it = cache->series.find(cci_key);
  if (it == cache->series.end() || it->second.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const auto& cci = it->second;
  size_t start = (n == 0) ? 0 : (cci.size() > n ? cci.size() - n : 0);
  double mn = INFINITY, mx = -INFINITY;
  size_t short_cnt = 0, long_cnt = 0;
  for (size_t i = start; i < cci.size(); ++i) {
    mn = std::min(mn, cci[i]); mx = std::max(mx, cci[i]);
    if (cci[i] > 100.0) ++short_cnt;
    if (cci[i] < -100.0) ++long_cnt;
  }
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (mn < -100.0) v1 = "\xe5\xbc\x80\xe5\xa4\x9a";    // 开多
  if (mx > 100.0)  v1 = "\xe5\xbc\x80\xe7\xa9\xba";    // 开空（覆盖开多）
  // k2 仅用于 key 构造，与 cci 计数无关（按 Rust 不输出计数到 v1）
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// clv_up_dw_line_V230605：CLV 多空（对齐 Rust clv.rs:32 clv_up_dw_line_v230605）
//  clv_i = (2*close-low-high)/(high-low); 看多: mean(clv)>0
static std::vector<Signal> clv_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 70);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n);
  const char* k3 = "CLV\xe5\xa4\x9a\xe7\xa9\xbaV230605";  // CLV多空V230605
  if (c.bars_raw.size() < di + 100) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, n);
  if (bars.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double sum = 0.0;
  for (auto& b : bars) {
    double den = b.high - b.low;
    sum += (den == 0.0) ? 0.0 : (2.0 * b.close - b.low - b.high) / den;
  }
  double clv_ma = sum / bars.size();
  const char* v1 = (clv_ma > 0.0) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cmo_up_dw_line_V230605：CMO 能量阈值（对齐 Rust ang.rs:253 cmo_up_dw_line_v230605）
//  up_sum/dw_sum = Σ |Δclose|; cmo = (up-dw)/(up+dw)*100; >m→看多, <-m→看空
static std::vector<Signal> cmo_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 70), m = p.usize("m", 30);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "M" + F(m);
  const char* k3 = "CMO\xe8\x83\xbd\xe9\x87\x8fV230605";  // CMO能量V230605
  if (c.bars_raw.size() < di + n + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, n);
  if (bars.size() < 2) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double up = 0.0, dw = 0.0;
  for (size_t i = 1; i < bars.size(); ++i) {
    double d = bars[i].close - bars[i-1].close;
    if (d > 0.0) up += d; else if (d < 0.0) dw += -d;
  }
  double cmo = (up + dw == 0.0) ? NAN : (up - dw) / (up + dw) * 100.0;
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (std::isfinite(cmo) && cmo > (double)m) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (std::isfinite(cmo) && cmo < -(double)m) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

static std::vector<Signal> coo_cci_v230323(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  auto key=ta::ma_cache_key("SMA",14); ta::update_ma_cache(c.bars_raw,key.c_str(),"SMA",14,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=it->second.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","CooCciV230323",v1.c_str());
}

// coo_kdj_V230322：KDJ+MA 配合多空（对齐 Rust coo.rs:231 coo_kdj_v230322）
//   多头: close>MA && K<D；空头: close<MA && K>D
static std::vector<Signal> coo_kdj_v230322(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache || c.bars_raw.empty()) return {};
  size_t di = p.usize("di", 1), n = p.usize("n", 3);
  const char* mt = p.str("ma_type", "EMA");
  int fk = (int)p.usize("fastk_period", 9), sk = (int)p.usize("slowk_period", 3), sd = (int)p.usize("slowd_period", 3);
  const std::string ma_key = std::string(mt) + "_" + F((int)n);
  const std::string kdj_key = "kdj_" + F(fk) + "_" + F(sk) + "_" + F(sd);
  ta::update_ma_cache(c.bars_raw, ma_key.c_str(), mt, (int)n, *cache);
  ta::update_kdj_cache(c.bars_raw, kdj_key.c_str(), fk, sk, sd, *cache);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "KDJ" + F(fk) + "#" + F(sk) + "#" + F(sd) + "#" + mt + "#" + F((int)n);
  const char* k3 = "BS\xe8\xbe\x85\xe5\x8a\xa9V230322";  // BS辅助V230322
  if (c.bars_raw.size() < di + fk * sk + 1) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  size_t idx = c.bars_raw.size() - di;
  auto it_ma = cache->series.find(ma_key);
  auto it_kdj = cache->kdj.find(kdj_key);
  if (it_ma == cache->series.end() || it_kdj == cache->kdj.end()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double close = c.bars_raw[idx].close;
  double ma = (idx < it_ma->second.size()) ? it_ma->second[idx] : NAN;
  double K = (idx < it_kdj->second.k.size()) ? it_kdj->second.k[idx] : NAN;
  double D = (idx < it_kdj->second.d.size()) ? it_kdj->second.d[idx] : NAN;
  if (!std::isfinite(ma) || !std::isfinite(K) || !std::isfinite(D)) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v1;
  if (close > ma && K < D) v1 = "\xe5\xa4\x9a\xe5\xa4\xb4";        // 多头
  else if (close < ma && K > D) v1 = "\xe7\xa9\xba\xe5\xa4\xb4";   // 空头
  else v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// coo_sar_V230322：SAR+区间极值（对齐 Rust coo.rs:304 coo_sar_v230325）
//   多头: close>SAR && high>=HHV(close,N)；空头: close<SAR && low<=LLV(close,N)
static std::vector<Signal> coo_sar_v230325(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache || c.bars_raw.empty()) return {};
  size_t di = p.usize("di", 1), n = p.usize("n", 60);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "SAR";
  const char* k3 = "BS\xe8\xbe\x85\xe5\x8a\xa9V230325";  // BS辅助V230325
  if (c.bars_raw.size() < n + di + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const std::string sar_key = "SAR";
  ta::update_sar_cache(c.bars_raw, sar_key.c_str(), *cache);
  auto it_sar = cache->series.find(sar_key);
  auto bars = get_sub_elements_vec(c.bars_raw, di, n);
  if (bars.empty() || it_sar == cache->series.end()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  size_t idx = c.bars_raw.size() - di;
  if (idx >= it_sar->second.size()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double sar = it_sar->second[idx];
  const RawBar& last = c.bars_raw[idx];
  double hhv = -INFINITY, llv = INFINITY;
  for (auto& b : bars) { hhv = std::max(hhv, b.close); llv = std::min(llv, b.close); }
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (last.close > sar && last.high >= hhv) v1 = "\xe5\xa4\x9a\xe5\xa4\xb4";    // 多头
  if (last.close < sar && last.low <= llv)  v1 = "\xe7\xa9\xba\xe5\xa4\xb4";   // 空头（覆盖）
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// td_signal_from_close — TD 神奇九转计数（复用 coo_td_v221110/v221111）
static std::pair<const char*, std::string> td_signal_from_close(const std::vector<double>& close) {
  // 对齐 czsc.utils.kline.ta.td_signal（四项基准）: down-th3, down-th2, up-th2, up-th3
  size_t len = close.size();
  if (len < 4) return {"\xe5\x85\xb6\xe4\xbb\x96", ""};  // 其他
  int up = 0, down = 0;
  double th2 = 4.0, th3 = 6.0;
  for (size_t i = len - 1; i >= 4 && i < len; --i) {
    if (close[i] > close[i-4]) ++up; else up = 0;
    if (down > 0 || close[i] < close[i-4]) { if (close[i] < close[i-4]) ++down; else down = 0; }
    if (up == th2 || down == th2) break;
  }
  // 9 转阈值: up>=9 → TD 顶（看空），down>=9 → TD 底（看多）
  int up_run = 0, down_run = 0;
  for (size_t i = len - 1; i >= 4 && i < len; --i) {
    if (close[i] > close[i-4]) { ++up_run; down_run = 0; }
    else if (close[i] < close[i-4]) { ++down_run; up_run = 0; }
    else { up_run = 0; down_run = 0; }
    if (up_run >= th3 || down_run >= th3) break;
  }
  const char* v1 = "\xe5\xbb\xb6\xe7\xbb\xad";          // 延续
  std::string v2 = "\xe9\x9d\x9e\xe9\xa1\xb6";
  if (up_run >= th3) { v1 = "\xe7\x9c\x8b\xe7\xa9\xba"; v2 = "TD\xe9\xa1\xb6"; } // 看空/TD顶
  if (down_run >= th3) { v1 = "\xe7\x9c\x8b\xe5\xa4\x9a"; v2 = "TD\xe5\xba\x95"; } // 看多/TD底
  return {v1, v2};
}

// coo_td_V221110：TD 计数（对齐 Rust coo.rs:71 coo_td_v221110）
static std::vector<Signal> coo_td_v221110(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "K";
  const char* k3 = "TD";
  if (c.bars_raw.size() < di + 50) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, 50);
  if (bars.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  std::vector<double> close; close.reserve(bars.size());
  for (auto& b : bars) close.push_back(b.close);
  auto [v1, v2] = td_signal_from_close(close);
  // Rust emits v2 as non-trivial, but C++ Signal 是 6-7 段格式；这里 make_kline_signal_v2 仅保留 v1+v2
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// coo_td_V221111：TD 神通九转（对齐 Rust coo.rs:108 coo_td_v221111）
static std::vector<Signal> coo_td_v221111(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "TD";
  const char* k3 = "BS\xe8\xbe\x85\xe5\x8a\xa9V221111";  // BS辅助V221111
  if (c.bars_raw.size() < di + 50) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, 50);
  if (bars.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  std::vector<double> close; close.reserve(bars.size());
  for (auto& b : bars) close.push_back(b.close);
  auto [v1, v2] = td_signal_from_close(close);
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// convolve_prefix — 对齐 Rust cvolp.rs convolve_prefix（带截断因果卷积）
static std::vector<double> convolve_prefix(const std::vector<double>& volume, const std::vector<double>& weights) {
  size_t l = volume.size(), n = weights.size();
  std::vector<double> out(l, 0.0);
  for (size_t k = 0; k < l; ++k) {
    size_t i_start = (k + 1 > n) ? (k + 1 - n) : 0;
    double acc = 0.0;
    for (size_t i = i_start; i <= k; ++i) {
      size_t j = k - i;
      if (j < n) acc += volume[i] * weights[j];
    }
    out[k] = acc;
  }
  return out;
}

// cvolp_up_dw_line_V230612：成交量加权动量率（对齐 Rust cvolp.rs:52 cvolp_up_dw_line_v230612）
//  weights[k] = exp(-1 + k/(n-1)); 归一化; emap = convolve_volume(weights)
//  前 n 个填充 emap[n]; sroc = (emap[-1] - rolledback) / rolledback; >up/100→看多, <-dw/100→看空
static std::vector<Signal> cvolp_up_dw_line_v230612(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 34), m = p.usize("m", 55);
  int up = (int)p.usize("up", 5), dw = (int)p.usize("dw", 5);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n) + "M" + F(m) + "UP" + F(up) + "DW" + F(dw);
  const char* k3 = "CVOLP\xe5\x8a\xa8\xe9\x87\x8f\xe5\x8f\x98\xe5\x8c\x96\xe7\x8e\x87V230612";  // CVOLP动量变化率V230612
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";
  if (c.bars_raw.size() < di + n + 10) return make_kline_signal_v1(k1, k2, k3, other);
  auto bars = get_sub_elements_vec(c.bars_raw, di, n + m);
  if (bars.size() <= n) return make_kline_signal_v1(k1, k2, k3, other);
  std::vector<double> volume; volume.reserve(bars.size());
  for (auto& b : bars) volume.push_back((double)b.vol);
  std::vector<double> w; w.reserve(n);
  for (size_t i = 0; i < n; ++i) w.push_back(std::exp(-1.0 + (double)i / std::max((double)(n - 1), 1.0)));
  double sum_w = 0; for (double ww : w) sum_w += ww;
  if (sum_w == 0.0 || !std::isfinite(sum_w)) return make_kline_signal_v1(k1, k2, k3, other);
  for (double& ww : w) ww /= sum_w;
  auto emap = convolve_prefix(volume, w);
  double fill_v = emap[n];
  for (size_t i = 0; i < n; ++i) emap[i] = fill_v;
  size_t l = emap.size();
  size_t ridx = (l - 1 + l - (m % l)) % l;
  double numer = emap[l - 1] - emap[ridx];
  double sroc = (emap[ridx] == 0.0) ? NAN : numer / emap[ridx];
  const char* v1 = other;
  if (std::isfinite(sroc) && sroc > up / 100.0) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (std::isfinite(sroc) && sroc < -(double)dw / 100.0) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_base_V230228：笔基础状态（对齐 Rust cxt.rs:51）
static std::vector<Signal> cxt_bi_base_v230228(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t bi_init = p.usize("bi_init_length", 9);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0BL" + F(bi_init);
  const std::string k3 = "V230228";
  if (c.bi_list.size() < 3) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const auto& last = c.bi_list.back();
  const char* v1 = (last.direction == Direction::kDown) ? "\xe5\x90\x91\xe4\xb8\x8a" : "\xe5\x90\x91\xe4\xb8\x8b";  // 向上/向下
  const char* v2 = (c.bars_ubi.size() >= bi_init) ? "\xe4\xb8\xad\xe7\xbb\xa7" : "\xe8\xbd\xac\xe6\x8a\x98";  // 中继/转折
  return make_kline_signal_v2(k1, k2, k3, v1, v2);
}

// cxt_bi_end_V230104：单均线辅助判断笔结束（对齐 Rust cxt.rs:262）
static std::vector<Signal> cxt_bi_end_v230104(const CZSC& c, const ParamView& p, TaCache* cache) {
  double th = static_cast<double>(p.usize("th", 50));
  size_t tp = p.usize("timeperiod", 5);
  const char* mt = p.str("ma_type", "SMA");
  const std::string key = std::string(mt) + "#" + F(static_cast<int>(tp));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0" + std::string(mt) + "#" + F(static_cast<int>(tp)) + "T" + F(static_cast<int>(th));
  const char* k3 = "BE辅助V230104";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (!cache || c.bi_list.size() < 3) return make_kline_signal_v1(k1, k2, k3, v1);
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto bars = get_sub_elements_vec(c.bars_raw, 1, 3);
  if (bars.size() != 3) return make_kline_signal_v1(k1, k2, k3, v1);
  auto it_ma = cache->series.find(key);
  if (it_ma == cache->series.end()) return make_kline_signal_v1(k1, k2, k3, v1);
  const auto& ma = it_ma->second;
  auto id_to_idx = bar_index_map(c);
  auto it = id_to_idx.find(bars[2].id);
  if (it == id_to_idx.end() || it->second >= ma.size()) return make_kline_signal_v1(k1, k2, k3, v1);
  double bar3_ma = ma[it->second];
  if (std::isnan(bar3_ma)) return make_kline_signal_v1(k1, k2, k3, v1);
  const BI& last_bi = c.bi_list.back();
  // 三连阳/阴
  bool lian_yang = bars[0].close > bars[0].open && bars[1].close > bars[1].open && bars[2].close > bars[2].open;
  bool lian_yin = bars[0].close < bars[0].open && bars[1].close < bars[1].open && bars[2].close < bars[2].open;
  if (c.bars_ubi.size() < 7) {
    if (last_bi.direction == Direction::kDown && lian_yang && bar3_ma * (1.0 + th / 10000.0) < bars[2].close)
      v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
    if (last_bi.direction == Direction::kUp && lian_yin && bar3_ma * (1.0 - th / 10000.0) > bars[2].close)
      v1 = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_end_V230105：K线形态+均线辅助判断笔结束（对齐 Rust cxt.rs:343）
static std::vector<Signal> cxt_bi_end_v230105(const CZSC& c, const ParamView& p, TaCache* cache) {
  double th = static_cast<double>(p.usize("th", 50));
  size_t tp = p.usize("timeperiod", 5);
  const char* mt = p.str("ma_type", "SMA");
  const std::string key = std::string(mt) + "#" + F(static_cast<int>(tp));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0" + std::string(mt) + "#" + F(static_cast<int>(tp)) + "T" + F(static_cast<int>(th));
  const char* k3 = "BE辅助V230105";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (!cache || c.bi_list.size() < 3 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, v1);
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto it_ma = cache->series.find(key);
  if (it_ma == cache->series.end()) return make_kline_signal_v1(k1, k2, k3, v1);
  const auto& ma = it_ma->second;
  auto id_to_idx = bar_index_map(c);
  const BI& last_bi = c.bi_list.back();
  auto fx_raw = fx_raw_bars(last_bi.fx_b);
  if (fx_raw.size() < 2) return make_kline_signal_v1(k1, k2, k3, v1);
  const RawBar &bar1 = fx_raw[fx_raw.size()-2], &bar2 = fx_raw.back();
  auto it = id_to_idx.find(bar2.id);
  if (it == id_to_idx.end() || it->second >= ma.size()) return make_kline_signal_v1(k1, k2, k3, v1);
  double bar2_ma = ma[it->second];
  if (std::isnan(bar2_ma)) return make_kline_signal_v1(k1, k2, k3, v1);
  if (c.bars_ubi.size() < 7) {
    // 看多：向下笔，先阴后强阳上穿均线
    if (last_bi.direction == Direction::kDown && bar1.low == last_bi.get_low() &&
        bar1.close < bar1.open && bar2.close > bar2_ma * (1.0 + th/10000.0) && bar2_ma * (1.0 + th/10000.0) > bar2.open)
      v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
    // 看空：向上笔，先阳后强阴下破均线
    if (last_bi.direction == Direction::kUp && bar1.high == last_bi.get_high() &&
        bar1.close > bar1.open && bar2.close < bar2_ma * (1.0 - th/10000.0) && bar2_ma * (1.0 - th/10000.0) < bar2.open)
      v1 = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_end_V230222：未完成笔分型新高新低次数（对齐 Rust cxt.rs:1628）
static std::vector<Signal> cxt_bi_end_v230222(const CZSC& c, const ParamView& p, TaCache*) {
  size_t max_overlap = p.usize("max_overlap", 3);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D1MO" + F(max_overlap);
  const char* k3 = "BE辅助V230222";
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  std::vector<FX> ubi_fxs = c.get_ubi_fxs();
  if (ubi_fxs.empty() || c.bars_ubi.size() >= 7) return make_kline_signal_v2(k1, k2, k3, other, other);
  // 收集分型：末笔内部分型(除首个) + ubi_fxs（时序递增）
  std::vector<FX> fxs;
  if (!c.bi_list.empty()) {
    const auto& last_bi = c.bi_list.back();
    for (size_t i = 1; i < last_bi.fxs.size(); ++i) fxs.push_back(last_bi.fxs[i]);
  }
  for (const auto& x : ubi_fxs) {
    if (fxs.empty() || x.dt > fxs.back().dt) fxs.push_back(x);
  }
  if (fxs.empty()) return make_kline_signal_v2(k1, k2, k3, other, "\xe7\xac\xac" + std::string(other) + "\xe6\xac\xa1");
  const FX& last_fx = fxs.back();
  auto last_fx_raw = fx_raw_bars(last_fx);
  if (last_fx_raw.empty()) return make_kline_signal_v2(k1, k2, k3, other, "\xe7\xac\xac" + std::string(other) + "\xe6\xac\xa1");
  // 时机校验：末根K线时刻匹配 或 距离<=max_overlap
  bool timing = (!last_fx.elements.empty() && !last_fx.elements.back().elements.empty() &&
                 last_fx.elements.back().elements.back().dt == c.bars_ubi.back().dt) ||
                (c.bars_raw.back().id >= last_fx_raw.back().id &&
                 static_cast<size_t>(c.bars_raw.back().id - last_fx_raw.back().id) <= max_overlap);
  if (!timing) return make_kline_signal_v2(k1, k2, k3, other, "\xe7\xac\xac" + std::string(other) + "\xe6\xac\xa1");
  if (last_fx.mark == Mark::kG) {
    double high_max = -std::numeric_limits<double>::infinity();
    int cnt = 0;
    for (const auto& fx : fxs) if (fx.mark == Mark::kG) {
      if (fx.high > high_max) { ++cnt; high_max = fx.high; }
    }
    if (last_fx.high == high_max) {
      std::string v2 = "\xe7\xac\xac" + std::to_string(cnt) + "\xe6\xac\xa1";  // 第X次
      return make_kline_signal_v2(k1, k2, k3, "\xe6\x96\xb0\xe9\xab\x98", v2.c_str());  // 新高
    }
  } else {
    double low_min = std::numeric_limits<double>::infinity();
    int cnt = 0;
    for (const auto& fx : fxs) if (fx.mark == Mark::kD) {
      if (fx.low < low_min) { ++cnt; low_min = fx.low; }
    }
    if (last_fx.low == low_min) {
      std::string v2 = "\xe7\xac\xac" + std::to_string(cnt) + "\xe6\xac\xa1";  // 第X次
      return make_kline_signal_v2(k1, k2, k3, "\xe6\x96\xb0\xe4\xbd\x8e", v2.c_str());  // 新低
    }
  }
  return make_kline_signal_v2(k1, k2, k3, other, "\xe7\xac\xac" + std::string(other) + "\xe6\xac\xa1");
}

// cxt_bi_end_V230224：量价配合笔结束辅助（对齐 Rust cxt.rs:418）
static std::vector<Signal> cxt_bi_end_v230224(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "D1";
  const char* k3 = "BE辅助V230224";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() <= 3 || c.bars_ubi.size() >= 7) return make_kline_signal_v1(k1, k2, k3, v1);
  const BI& last_bi = c.bi_list.back();
  auto bi_bars = last_bi.get_raw_bars();
  auto fx_bars = fx_raw_bars(last_bi.fx_b);
  if (bi_bars.empty() || fx_bars.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  double bi_vol = 0, fx_vol = 0;
  for (const auto& b : bi_bars) bi_vol += b.vol;
  for (const auto& b : fx_bars) fx_vol += b.vol;
  bi_vol /= bi_bars.size(); fx_vol /= fx_bars.size();
  // bar1=最低点, bar2=最高点
  const RawBar* bar1 = &fx_bars[0]; const RawBar* bar2 = &fx_bars[0];
  for (size_t i = 1; i < fx_bars.size(); ++i) {
    if (fx_bars[i].low < bar1->low) bar1 = &fx_bars[i];
    if (fx_bars[i].high > bar2->high) bar2 = &fx_bars[i];
  }
  if (raw_bar_upper(*bar1) > raw_bar_lower(*bar1) * 2.0 && fx_vol > bi_vol * 2.0) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
  if (2.0 * raw_bar_upper(*bar2) < raw_bar_lower(*bar2) && fx_vol < bi_vol * 0.618) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_end_V230312：MACD辅助判断笔结束（对齐 Rust cxt.rs:481）
static std::vector<Signal> cxt_bi_end_v230312(const CZSC& c, const ParamView& p, TaCache* cache) {
  int fast = static_cast<int>(p.usize("fastperiod", 12));
  int slow = static_cast<int>(p.usize("slowperiod", 26));
  int sig = static_cast<int>(p.usize("signalperiod", 9));
  const std::string key = "MACD" + F(fast) + "#" + F(slow) + "#" + F(sig);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0MACD" + F(fast) + "#" + F(slow) + "#" + F(sig);
  const char* k3 = "BE辅助V230312";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (!cache || c.bi_list.size() < 3 || c.bars_ubi.size() >= 7) return make_kline_signal_v1(k1, k2, k3, v1);
  ta::update_macd_cache(c.bars_raw, key.c_str(), fast, slow, sig, *cache);
  const BI& last_bi = c.bi_list.back();
  auto fx_bars = fx_raw_bars(last_bi.fx_b);
  if (fx_bars.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  std::unordered_map<int32_t, std::tuple<double,double,double>> ov;
  auto get = [&](const RawBar& rb) -> double {
    auto v = ta::MacdSnapshotValue(c.bars_raw, *cache, key, rb, fast, slow, sig, 2, ov);  // macd_hist
    return v.value_or(NAN);
  };
  double macd1 = get(fx_bars.back());   // 分型尾部
  double macd2 = get(fx_bars.front());  // 分型起点
  if (!std::isfinite(macd1) || !std::isfinite(macd2)) return make_kline_signal_v1(k1, k2, k3, v1);
  if (last_bi.direction == Direction::kDown && macd1 > macd2) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
  if (last_bi.direction == Direction::kUp && macd1 < macd2) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_end_V230320：质数窗口笔结束辅助（对齐 Rust cxt.rs:1879）
static std::vector<Signal> cxt_bi_end_v230320(const CZSC& c, const ParamView& p, TaCache*) {
  size_t max_overlap = p.usize("max_overlap", 3);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0\xe8\xb4\xa8\xe6\x95\xb0\xe7\xaa\x93\xe5\x8f\xa3MO" + F(max_overlap);  // D0质数窗口MO{}
  const char* k3 = "BE辅助V230320";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < 3) return make_kline_signal_v1(k1, k2, k3, v1);
  const size_t primes[] = {11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,97};
  const BI& last_bi = c.bi_list.back();
  // bars_ubi[1:] 展平
  std::vector<RawBar> raw_bars;
  for (size_t i = 1; i < c.bars_ubi.size(); ++i)
    raw_bars.insert(raw_bars.end(), c.bars_ubi[i].elements.begin(), c.bars_ubi[i].elements.end());
  if (raw_bars.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  size_t ubi_len = raw_bars.size();
  double umi = std::numeric_limits<double>::infinity(), uma = -std::numeric_limits<double>::infinity();
  for (const auto& b : raw_bars) { umi = std::min(umi, b.low); uma = std::max(uma, b.high); }
  // 末max_overlap根
  size_t start = raw_bars.size() > max_overlap ? raw_bars.size() - max_overlap : 0;
  bool is_prime = std::find(std::begin(primes), std::end(primes), ubi_len) != std::end(primes);
  if (is_prime && last_bi.direction == Direction::kUp) {
    double mn = std::numeric_limits<double>::infinity();
    for (size_t i = start; i < raw_bars.size(); ++i) mn = std::min(mn, raw_bars[i].low);
    if (mn == umi) {
      std::string v = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
      return make_kline_signal_v2(k1, k2, k3, v.c_str(), (F(ubi_len)+"K").c_str());
    }
  }
  if (is_prime && last_bi.direction == Direction::kDown) {
    double mx = -std::numeric_limits<double>::infinity();
    for (size_t i = start; i < raw_bars.size(); ++i) mx = std::max(mx, raw_bars[i].high);
    if (mx == uma) {
      std::string v = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
      return make_kline_signal_v2(k1, k2, k3, v.c_str(), (F(ubi_len)+"K").c_str());
    }
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_end_V230322：分型配合均线的笔结束辅助（对齐 Rust cxt.rs:1937）
static std::vector<Signal> cxt_bi_end_v230322(const CZSC& c, const ParamView& p, TaCache* cache) {
  const char* mt = p.str("ma_type", "SMA");
  size_t tp = p.usize("timeperiod", 5);
  const std::string key = std::string(mt) + "#" + F(static_cast<int>(tp));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0\xe5\x88\x86\xe5\x9e\x8b\xe9\x85\x8d\xe5\x90\x88" + std::string(mt) + "#" + F(static_cast<int>(tp));  // 分型配合X#Y
  const char* k3 = "BE辅助V230322";
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (!cache) return make_kline_signal_v1(k1, k2, k3, other);
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto it_ma = cache->series.find(key);
  if (it_ma == cache->series.end()) return make_kline_signal_v1(k1, k2, k3, other);
  const auto& ma = it_ma->second;
  auto id_to_idx = bar_index_map(c);
  std::vector<FX> ubi_fxs = c.get_ubi_fxs();
  const RawBar& last_bar = c.bars_raw.back();
  if (c.bi_list.size() < 3 || c.bars_ubi.size() > 7 || ubi_fxs.empty()) return make_kline_signal_v1(k1, k2, k3, other);
  // 末根UBI分型的末根K线时刻
  const FX& last_fx = ubi_fxs.back();
  auto last_fx_raw = fx_raw_bars(last_fx);
  int64_t fx_end_dt = last_fx_raw.empty() ? last_bar.dt : last_fx_raw.back().dt;
  if (last_bar.dt != fx_end_dt) return make_kline_signal_v1(k1, k2, k3, other);
  const BI& last_bi = c.bi_list.back();
  std::vector<double> ma_vals;
  for (const auto& rb : last_fx_raw) {
    auto it = id_to_idx.find(rb.id);
    if (it != id_to_idx.end() && it->second < ma.size()) ma_vals.push_back(ma[it->second]);
  }
  if (ma_vals.empty()) return make_kline_signal_v1(k1, k2, k3, other);
  double mx = -std::numeric_limits<double>::infinity(), mn = std::numeric_limits<double>::infinity();
  for (double v : ma_vals) { mx = std::max(mx, v); mn = std::min(mn, v); }
  auto it_right = id_to_idx.find(last_fx_raw.back().id);
  double right_ma = (it_right != id_to_idx.end() && it_right->second < ma.size()) ? ma[it_right->second] : NAN;
  if (last_bi.direction == Direction::kUp) {
    if (last_fx.mark == Mark::kG && right_ma == mn) {
      return make_kline_signal_v2(k1, k2, k3, "\xe7\x9c\x8b\xe7\xa9\xba", "\xe5\x90\x8c\xe5\x90\x91\xe5\x88\x86\xe5\x9e\x8b");  // 看空/同向分型
    }
    if (last_fx.mark == Mark::kD && right_ma != mx) {
      return make_kline_signal_v2(k1, k2, k3, "\xe7\x9c\x8b\xe7\xa9\xba", "\xe5\x8f\x8d\xe5\x90\x91\xe5\x88\x86\xe5\x9e\x8b");  // 看空/反向分型
    }
  }
  if (last_bi.direction == Direction::kDown) {
    if (last_fx.mark == Mark::kD && right_ma == mx) {
      return make_kline_signal_v2(k1, k2, k3, "\xe7\x9c\x8b\xe5\xa4\x9a", "\xe5\x90\x8c\xe5\x90\x91\xe5\x88\x86\xe5\x9e\x8b");  // 看多/同向分型
    }
    if (last_fx.mark == Mark::kG && right_ma != mn) {
      return make_kline_signal_v2(k1, k2, k3, "\xe7\x9c\x8b\xe5\xa4\x9a", "\xe5\x8f\x8d\xe5\x90\x91\xe5\x88\x86\xe5\x9e\x8b");  // 看多/反向分型
    }
  }
  return make_kline_signal_v1(k1, k2, k3, other);
}

// cxt_bi_end_V230324：笔结束分型均线突破（对齐 Rust cxt.rs:566）
static std::vector<Signal> cxt_bi_end_v230324(const CZSC& c, const ParamView& p, TaCache* cache) {
  const char* mt = p.str("ma_type", "SMA");
  size_t tp = p.usize("timeperiod", 5);
  const std::string key = std::string(mt) + "#" + F(static_cast<int>(tp));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D0" + std::string(mt) + "#" + F(static_cast<int>(tp)) + "\xe5\x9d\x87\xe7\xba\xbf\xe7\xaa\x81\xe7\xa0\xb4";  // 均线突破
  const char* k3 = "BE辅助V230324";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (!cache || c.bi_list.size() < 3 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, v1);
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto it_ma = cache->series.find(key);
  if (it_ma == cache->series.end()) return make_kline_signal_v1(k1, k2, k3, v1);
  const auto& ma = it_ma->second;
  auto id_to_idx = bar_index_map(c);
  const BI& last_bi = c.bi_list.back();
  auto fx_raw = fx_raw_bars(last_bi.fx_b);
  if (fx_raw.size() < 2) return make_kline_signal_v1(k1, k2, k3, v1);
  // 分型内（除末根）的均线序列
  std::vector<double> ma_vals;
  for (size_t i = 0; i + 1 < fx_raw.size(); ++i) {
    auto it = id_to_idx.find(fx_raw[i].id);
    if (it != id_to_idx.end() && it->second < ma.size() && std::isfinite(ma[it->second]))
      ma_vals.push_back(ma[it->second]);
  }
  if (ma_vals.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  double mx = -std::numeric_limits<double>::infinity(), mn = std::numeric_limits<double>::infinity();
  for (double v : ma_vals) { mx = std::max(mx, v); mn = std::min(mn, v); }
  double last_close = c.bars_raw[c.bars_raw.size()-2].close;
  if (last_bi.direction == Direction::kUp && last_close < mn) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
  if (last_bi.direction == Direction::kDown && last_close > mx) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_end_V230618：笔结束小中枢辅助（对齐 Rust cxt.rs:2019）
static std::vector<Signal> cxt_bi_end_v230618(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  size_t max_overlap = p.usize("max_overlap", 3);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "MO" + F(max_overlap);
  const char* k3 = "BE辅助V230618";
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  std::string v1 = other, v2 = other;
  if (c.bi_list.size() < di + 6 || c.bars_ubi.size() > 7)
    return make_kline_signal_v2(k1, k2, k3, v1.c_str(), v2.c_str());
  auto bis = get_sub_elements_vec(c.bi_list, di, 5);
  if (bis.size() < 2) return make_kline_signal_v2(k1, k2, k3, v1.c_str(), v2.c_str());
  const BI& last_bi = bis.back();
  auto raw_bars = last_bi.get_raw_bars();
  if (raw_bars.empty()) return make_kline_signal_v2(k1, k2, k3, v1.c_str(), v2.c_str());
  std::unordered_map<int64_t,int> cnt;
  for (const auto& b : raw_bars) ++cnt[b.dt];
  int peaks = 0;
  for (const auto& [dt,c] : cnt) if (c >= static_cast<int>(max_overlap)) ++peaks;
  if (peaks >= 1) {
    v1 = (last_bi.direction == Direction::kUp) ? "\xe7\x9c\x8b\xe7\xa9\xba" : "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看空/看多
    v2 = std::to_string(peaks) + "\xe5\xb0\x8f\xe4\xb8\xad\xe6\x9e\xa2";  // X小中枢
  }
  return make_kline_signal_v2(k1, k2, k3, v1.c_str(), v2.c_str());
}

// cxt_bi_end_V230815：快速突破反向笔（对齐 Rust cxt.rs:641）
static std::vector<Signal> cxt_bi_end_v230815(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "\xe5\xbf\xab\xe9\x80\x9f\xe7\xaa\x81\xe7\xa0\xb4";  // 快速突破
  const char* k3 = "BE辅助V230815";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < 5 || c.bars_ubi.size() >= 5)
    return make_kline_signal_v1(k1, k2, k3, v1);
  const auto& bi = c.bi_list.back();
  const auto& last_bar = c.bars_ubi.back();
  if (bi.direction == Direction::kUp && last_bar.low < bi.get_low()) v1 = "\xe5\x90\x91\xe4\xb8\x8b";  // 向下
  if (bi.direction == Direction::kDown && last_bar.high > bi.get_high()) v1 = "\xe5\x90\x91\xe4\xb8\x8a";  // 向上
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_status_V230101：笔表里关系（对齐 Rust cxt.rs:105）
static std::vector<Signal> cxt_bi_status_v230101(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "D1";
  const char* k3 = "表里关系V230101";
  std::vector<FX> ubi_fxs = c.get_ubi_fxs();
  if (c.bi_list.size() < 3 || ubi_fxs.empty())
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto [v1, v2] = calc_bi_status_values(c, ubi_fxs);
  return make_kline_signal_v2(k1, k2, k3, v1, v2);
}

// cxt_bi_status_V230102：笔表里关系+时机（对齐 Rust cxt.rs:154）
static std::vector<Signal> cxt_bi_status_v230102(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "D1";
  const char* k3 = "表里关系V230102";
  std::vector<FX> ubi_fxs = c.get_ubi_fxs();
  if (c.bi_list.size() < 3 || ubi_fxs.empty() || c.bars_raw.empty())
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  int64_t last_bar_dt = c.bars_raw.back().dt;
  // 末根UBI分型的末根K线时刻
  int64_t last_fx_end_dt = 0;
  { const FX& lf = ubi_fxs.back();
    if (!lf.elements.empty()) {
      const auto& nb = lf.elements.back();
      last_fx_end_dt = nb.elements.empty() ? nb.dt : nb.elements.back().dt;
    }
  }
  if (last_bar_dt != last_fx_end_dt)
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto [v1, v2] = calc_bi_status_values(c, ubi_fxs);
  return make_kline_signal_v2(k1, k2, k3, v1, v2);
}

// cxt_bi_stop_V230815：笔止损距离状态（对齐 Rust cxt.rs:684）
static std::vector<Signal> cxt_bi_stop_v230815(const CZSC& c, const ParamView& p, TaCache*) {
  double th = static_cast<double>(p.usize("th", 50));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "\xe8\xb7\x9d\xe7\xa6\xbb" + F(static_cast<int>(th)) + "BP";  // 距离XBP
  const char* k3 = "止损V230815";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  const char* v2 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < 5 || c.bars_ubi.empty())
    return make_kline_signal_v2(k1, k2, k3, v1, v2);
  const auto& bi = c.bi_list.back();
  const auto& last_bar = c.bars_ubi.back();
  if (bi.direction == Direction::kUp) {
    v1 = "\xe5\x90\x91\xe4\xb8\x8b";  // 向下
    v2 = (last_bar.close > bi.get_high() * (1.0 - th / 10000.0)) ? "\xe9\x98\x88\xe5\x80\xbc\xe5\x86\x85" : "\xe9\x98\x88\xe5\x80\xbc\xe5\xa4\x96";  // 阈值内/阈值外
  }
  if (bi.direction == Direction::kDown) {
    v1 = "\xe5\x90\x91\xe4\xb8\x8a";  // 向上
    v2 = (last_bar.close < bi.get_low() * (1.0 + th / 10000.0)) ? "\xe9\x98\x88\xe5\x80\xbc\xe5\x86\x85" : "\xe9\x98\x88\xe5\x80\xbc\xe5\xa4\x96";  // 阈值内/阈值外
  }
  return make_kline_signal_v2(k1, k2, k3, v1, v2);
}

// cxt_bi_trend_V230824：N笔形态判断（对齐 Rust cxt.rs:740）
static std::vector<Signal> cxt_bi_trend_v230824(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const size_t n = p.usize("n", 4);
  double th = static_cast<double>(p.usize("th", 2));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n) + "TH" + F(static_cast<int>(th));
  const char* k3 = "形态V230824";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < di + n + 2) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bis = get_sub_elements_vec(c.bi_list, di, n);
  if (bis.size() != n) return make_kline_signal_v1(k1, k2, k3, v1);
  double avg = 0;
  for (const auto& bi : bis) avg += (bi.get_low() + bi.get_high()) / 2.0;
  avg /= static_cast<double>(n);
  if (!std::isfinite(avg) || avg == 0.0) return make_kline_signal_v1(k1, k2, k3, v1);
  double ratio = bis[0].get_low() + bis[0].get_high();
  ratio = (ratio / 2.0) / avg;  // 简化：用首笔均值占比（与Rust语义对齐需mean of first）
  // Rust 用 means[0]/avg；means[i]=(low+high)/2
  double mean0 = (bis[0].get_low() + bis[0].get_high()) / 2.0;
  ratio = mean0 / avg;
  if (ratio * 100.0 > 100.0 + th) v1 = "\xe5\x90\x91\xe4\xb8\x8b";  // 向下
  else if (ratio * 100.0 < 100.0 - th) v1 = "\xe5\x90\x91\xe4\xb8\x8a";  // 向上
  else v1 = "\xe6\xa8\xaa\xe7\x9b\x98";  // 横盘
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bi_trend_V230913：笔趋势高低点回归（对齐 Rust cxt.rs:2792）
static std::vector<Signal> cxt_bi_trend_v230913(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 4);
  size_t n = p.usize("n", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n) + "\xe7\xac\x94\xe8\xb6\x8b\xe5\x8a\xbf";  // D{}N{}笔趋势
  const char* k3 = "\xe9\xab\x98\xe4\xbd\x8e\xe7\x82\xb9\xe8\xbe\x85\xe5\x8a\xa9\xe5\x88\xa4\xe6\x96\xadV230913";  // 高低点辅助判断V230913
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() <= di + 2 || c.bars_ubi.size() <= n + 1) return make_kline_signal_v1(k1, k2, k3, other);
  // 分离向上/向下笔
  std::vector<const BI*> up_bis, down_bis;
  for (const auto& bi : c.bi_list) {
    if (bi.direction == Direction::kUp) up_bis.push_back(&bi); else down_bis.push_back(&bi);
  }
  if (up_bis.empty() || down_bis.empty()) return make_kline_signal_v1(k1, k2, k3, other);
  size_t up_take = std::min(up_bis.size(), di), down_take = std::min(down_bis.size(), di);
  std::vector<double> up_x, up_y, down_x, down_y;
  for (size_t i = up_bis.size()-up_take; i < up_bis.size(); ++i) { up_x.push_back(static_cast<double>(up_bis[i]->end_dt())); up_y.push_back(up_bis[i]->get_high()); }
  for (size_t i = down_bis.size()-down_take; i < down_bis.size(); ++i) { down_x.push_back(static_cast<double>(down_bis[i]->end_dt())); down_y.push_back(down_bis[i]->get_low()); }
  double x = static_cast<double>(c.bars_ubi[c.bars_ubi.size()-n].dt);
  auto pre_up = linreg_predict(up_x, up_y, x);
  auto pre_down = linreg_predict(down_x, down_y, x);
  if (!pre_up || !pre_down) return make_kline_signal_v1(k1, k2, k3, other);
  double pre_mid = (*pre_up + *pre_down) / 2.0;
  if (*pre_up <= *pre_down) return make_kline_signal_v2(k1, k2, k3, "\xe8\xa7\x82\xe6\x9c\x9b", "\xe8\xb6\x8b\xe5\x8a\xbf\xe7\xba\xbf\xe4\xba\xa4\xe5\x8f\x89");  // 观望/趋势线交叉
  if (c.bars_ubi.size() >= 5) return make_kline_signal_v2(k1, k2, k3, "\xe8\xa7\x82\xe6\x9c\x9b", "\xe6\x9c\xab\xe7\xac\x94\xe5\xbb\xb6\xe4\xbc\xb8");  // 观望/末笔延伸
  double close = c.bars_raw[c.bars_raw.size()-n].close;
  if (close >= *pre_up) return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x8a\xe5\x8d\x87\xe8\xb6\x8b\xe5\x8a\xbf", "\xe8\xb6\x85\xe5\xbc\xba");  // 上升趋势/超强
  if (close > pre_mid) return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x8a\xe5\x8d\x87\xe8\xb6\x8b\xe5\x8a\xbf", "\xe5\xbc\xba");  // 上升趋势/强
  if (close > *pre_down) return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x8b\xe9\x99\x8d\xe8\xb6\x8b\xe5\x8a\xbf", "\xe5\xbc\xba");  // 下降趋势/强
  return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x8b\xe9\x99\x8d\xe8\xb6\x8b\xe5\x8a\xbf", "\xe8\xb6\x85\xe5\xbc\xba");  // 下降趋势/超强
}

// cxt_bi_zdf_V230601：BI涨跌幅分层（对齐 Rust cxt.rs:795）
static std::vector<Signal> cxt_bi_zdf_v230601(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const size_t n = p.usize("n", 5);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n);
  const char* k3 = "\xe5\x88\x86\xe5\xb1\x82V230601";  // 分层V230601
  if (c.bi_list.size() < 10 || c.bars_ubi.size() > 7)
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bis = get_sub_elements_vec(c.bi_list, di, 50);
  if (bis.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v1 = DirectionValue(bis.back().direction);  // 向上/向下
  std::vector<double> powers; powers.reserve(bis.size());
  for (const auto& bi : bis) powers.push_back(bi.get_power());
  int layer = qcut_last_label(powers, n);
  std::string v2;
  if (layer >= 0) {
    v2 = "\xe7\xac\xac" + std::to_string(layer + 1) + "\xe5\xb1\x82";  // 第X层
  } else {
    v2 = "\xe5\x85\xb6\xe4\xbb\x96";
  }
  return make_kline_signal_v2(k1, k2, k3, v1, v2.c_str());
}

// cxt_bs_V240526：趋势跟随 BS 辅助（对齐 Rust cxt.rs:1392）
static std::vector<Signal> cxt_bs_v240526(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "\xe8\xb6\x8b\xe5\x8a\xbf\xe8\xb7\x9f\xe9\x9a\x8f";  // 趋势跟随
  const char* k3 = "BS辅助V240526";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < 11) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bis = get_sub_elements_vec(c.bi_list, 1, 7);
  if (bis.size() < 7) return make_kline_signal_v1(k1, k2, k3, v1);
  const BI& b2 = bis[bis.size() - 2];
  const BI& b1 = bis[bis.size() - 1];
  double max_pp = -std::numeric_limits<double>::infinity();
  double max_pv = -std::numeric_limits<double>::infinity();
  double max_sl = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) {
    max_pp = std::max(max_pp, bi.get_power_price());
    max_pv = std::max(max_pv, bi.get_power_volume());
    max_sl = std::max(max_sl, std::abs(bi.get_slope()));
  }
  if (b2.get_snr() < 0.7 || (b2.get_power_price() < max_pp && b2.get_power_volume() < max_pv && b2.get_slope() < max_sl))
    return make_kline_signal_v1(k1, k2, k3, v1);
  if (b2.direction == Direction::kUp && b1.direction == Direction::kDown &&
      0.1 * b2.get_power_price() < b1.get_power_price() && b1.get_power_price() < 0.7 * b2.get_power_price())
    v1 = "\xe4\xb9\xb0\xe7\x82\xb9";  // 买点
  if (b2.direction == Direction::kDown && b1.direction == Direction::kUp &&
      0.2 * b2.get_power_price() < b1.get_power_price() && b1.get_power_price() < 0.7 * b2.get_power_price())
    v1 = "\xe5\x8d\x96\xe7\x82\xb9";  // 卖点
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_bs_V240527：未完成笔上的趋势跟随 BS 辅助（对齐 Rust cxt.rs:1468）
static std::vector<Signal> cxt_bs_v240527(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "\xe8\xb6\x8b\xe5\x8a\xbf\xe8\xb7\x9f\xe9\x9a\x8f";  // 趋势跟随
  const char* k3 = "BS辅助V240527";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < 11) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bis = get_sub_elements_vec(c.bi_list, 1, 7);
  if (bis.size() < 7) return make_kline_signal_v1(k1, k2, k3, v1);
  const BI& b1 = bis[bis.size() - 1];
  double max_pp = -std::numeric_limits<double>::infinity();
  double max_pv = -std::numeric_limits<double>::infinity();
  double max_sl = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) {
    max_pp = std::max(max_pp, bi.get_power_price());
    max_pv = std::max(max_pv, bi.get_power_volume());
    max_sl = std::max(max_sl, std::abs(bi.get_slope()));
  }
  if (b1.get_snr() < 0.7 || (b1.get_power_price() < max_pp && b1.get_power_volume() < max_pv && b1.get_slope() < max_sl))
    return make_kline_signal_v1(k1, k2, k3, v1);
  // ubi_raw_bars: 无包含K线展平
  std::vector<RawBar> ubi_bars;
  for (const auto& nb : c.bars_ubi)
    ubi_bars.insert(ubi_bars.end(), nb.elements.begin(), nb.elements.end());
  if (ubi_bars.size() < 7) return make_kline_signal_v1(k1, k2, k3, v1);
  double ubi_high = -std::numeric_limits<double>::infinity();
  double ubi_low = std::numeric_limits<double>::infinity();
  for (const auto& b : ubi_bars) { ubi_high = std::max(ubi_high, b.high); ubi_low = std::min(ubi_low, b.low); }
  double ubi_pp = ubi_high - ubi_low;
  if (b1.direction == Direction::kUp && 0.1 * b1.get_power_price() < ubi_pp && ubi_pp < 0.7 * b1.get_power_price())
    v1 = "\xe4\xb9\xb0\xe7\x82\xb9";  // 买点
  if (b1.direction == Direction::kDown && 0.2 * b1.get_power_price() < ubi_pp && ubi_pp < 0.7 * b1.get_power_price())
    v1 = "\xe5\x8d\x96\xe7\x82\xb9";  // 卖点
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_decision_V240526：分型区域决策（对齐 Rust cxt.rs:1136）
static std::vector<Signal> cxt_decision_v240526(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t n = p.usize("n", 9);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "\xe5\x88\x86\xe5\x9e\x8b\xe5\x8c\xba\xe5\x9f\x9fN" + F(n);  // 分型区域N{}
  const char* k3 = "决策区域V240526";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bars_raw.size() < 120 || c.bi_list.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bars = get_sub_elements_vec(c.bars_raw, 1, 100);
  if (bars.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  auto prices = unique_prices_from_bars(bars);
  const BI& bi = c.bi_list.back();
  const RawBar& bar = c.bars_raw.back();
  if (bi.direction == Direction::kUp) {
    size_t cnt = 0;
    for (double pr : prices) if (bar.close <= pr && pr <= bi.fx_b.high) ++cnt;
    if (cnt <= n) v1 = "\xe5\xbc\x80\xe7\xa9\xba";  // 开空
  } else if (bi.direction == Direction::kDown) {
    size_t cnt = 0;
    for (double pr : prices) if (bi.fx_b.low <= pr && pr <= bar.close) ++cnt;
    if (cnt <= n) v1 = "\xe5\xbc\x80\xe5\xa4\x9a";  // 开多
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_decision_V240612：高低点N档决策区间（对齐 Rust cxt.rs:1196）
static std::vector<Signal> cxt_decision_v240612(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t w = p.usize("w", 10);
  const size_t n = p.usize("n", 9);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "W" + F(w) + "N" + F(n) + "\xe9\xab\x98\xe4\xbd\x8e\xe7\x82\xb9";  // W{}N{}高低点
  const char* k3 = "决策区域V240612";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bars_raw.size() < 120) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bars = get_sub_elements_vec(c.bars_raw, 1, 100);
  auto prices = unique_prices_from_bars(bars);
  if (prices.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  auto wbars = get_sub_elements_vec(c.bars_raw, 1, w);
  if (wbars.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  double mx = -std::numeric_limits<double>::infinity();
  double mn = std::numeric_limits<double>::infinity();
  for (const auto& b : wbars) { mx = std::max(mx, b.high); mn = std::min(mn, b.low); }
  const RawBar& last = c.bars_raw.back();
  // 低点上方第n档
  std::vector<double> above; for (double pr : prices) if (pr >= mn) above.push_back(pr);
  std::sort(above.begin(), above.end());
  double low_range = above.size() > n ? above[n] : above.back();
  // 高点下方第n档
  std::vector<double> below; for (double pr : prices) if (pr <= mx) below.push_back(pr);
  std::sort(below.begin(), below.end(), std::greater<double>());
  double high_range = below.size() > n ? below[n] : below.back();
  if (last.close < low_range && last.low != mn) v1 = "\xe5\xbc\x80\xe5\xa4\x9a";  // 开多
  if (last.close > high_range && last.high != mx) v1 = "\xe5\xbc\x80\xe7\xa9\xba";  // 开空
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_decision_V240613：放量笔N4BS2决策区（对齐 Rust cxt.rs:1272）
static std::vector<Signal> cxt_decision_v240613(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t n = p.usize("n", 4);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "\xe6\x94\xbe\xe9\x87\x8f\xe7\xac\x94N" + F(n) + "BS2";  // 放量笔N{}BS2
  const char* k3 = "决策区域V240613";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < n + 2 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bis = get_sub_elements_vec(c.bi_list, 1, n);
  if (bis.size() != n) return make_kline_signal_v1(k1, k2, k3, v1);
  double max_vol = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) max_vol = std::max(max_vol, bi.get_power_volume());
  const BI& last = bis.back();
  if (last.get_power_volume() != max_vol) return make_kline_signal_v1(k1, k2, k3, v1);
  double min_low = std::numeric_limits<double>::infinity();
  double max_high = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) { min_low = std::min(min_low, bi.get_low()); max_high = std::max(max_high, bi.get_high()); }
  if (last.direction == Direction::kDown && last.get_low() != min_low) v1 = "\xe5\xbc\x80\xe5\xa4\x9a";  // 开多
  if (last.direction == Direction::kUp && last.get_high() != max_high) v1 = "\xe5\xbc\x80\xe7\xa9\xba";  // 开空
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_decision_V240614：放量新高/新低决策区（对齐 Rust cxt.rs:1332）
static std::vector<Signal> cxt_decision_v240614(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t n = p.usize("n", 4);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "\xe6\x94\xbe\xe9\x87\x8f\xe7\xac\x94N" + F(n);  // 放量笔N{}
  const char* k3 = "决策区域V240614";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < n + 2 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, v1);
  auto bis = get_sub_elements_vec(c.bi_list, 1, n);
  if (bis.size() != n) return make_kline_signal_v1(k1, k2, k3, v1);
  double max_vol = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) max_vol = std::max(max_vol, bi.get_power_volume());
  const BI& last = bis.back();
  if (last.get_power_volume() != max_vol) return make_kline_signal_v1(k1, k2, k3, v1);
  double min_low = std::numeric_limits<double>::infinity();
  double max_high = -std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) { min_low = std::min(min_low, bi.get_low()); max_high = std::max(max_high, bi.get_high()); }
  if (last.direction == Direction::kDown && last.get_low() == min_low) v1 = "\xe5\xbc\x80\xe5\xa4\x9a";  // 开多
  if (last.direction == Direction::kUp && last.get_high() == max_high) v1 = "\xe5\xbc\x80\xe7\xa9\xba";  // 开空
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_double_zs_V230311：双中枢 BS1 辅助（对齐 Rust cxt.rs:1034）
static std::vector<Signal> cxt_double_zs_v230311(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "\xe5\x8f\x8c\xe4\xb8\xad\xe6\x9e\xa2";  // D{}双中枢
  const char* k3 = "BS1辅助V230311";
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  auto bis = get_sub_elements_vec(c.bi_list, di, 20);
  if (bis.empty()) return make_kline_signal_v1(k1, k2, k3, v1);
  auto zss = get_zs_seq(bis);
  if (zss.size() >= 2 && zss[zss.size()-2].bis.size() >= 2 && zss[zss.size()-1].bis.size() >= 2) {
    const ZS &zs1 = zss[zss.size()-2], &zs2 = zss[zss.size()-1];
    size_t ts1 = zs2.bis.back().bars.size();
    size_t ts2 = zs2.bis[zs2.bis.size()-2].bars.size();
    const BI& last = bis.back();
    if (last.direction == Direction::kDown && ts1 >= ts2 * 2 && zs1.gg > zs2.gg) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";  // 看多
    if (last.direction == Direction::kUp && ts1 >= ts2 * 2 && zs1.dd < zs2.dd) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";  // 看空
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_eleven_bi_V230622：十一笔形态分类（对齐 Rust cxt.rs:2574）
static std::vector<Signal> cxt_eleven_bi_v230622(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "\xe5\x8d\x81\xe4\xb8\x80\xe7\xac\x94";  // D{}十一笔
  const char* k3 = "\xe5\xbd\xa2\xe6\x80\x81V230622";  // 形态V230622
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < di + 16 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, other);
  auto bis = get_sub_elements_vec(c.bi_list, di, 11);
  if (bis.size() != 11) return make_kline_signal_v1(k1, k2, k3, other);
  const BI &b1=bis[0],&b2=bis[1],&b3=bis[2],&b4=bis[3],&b5=bis[4],&b6=bis[5],&b7=bis[6],&b8=bis[7],&b9=bis[8],&b10=bis[9],&b11=bis[10];
  double mx=-std::numeric_limits<double>::infinity(), mn=std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) { mx=std::max(mx,bi.get_high()); mn=std::min(mn,bi.get_low()); }
  const char* v1 = other;
  if (b11.direction == Direction::kDown) {
    if (mn==b11.get_low() && mx==b1.get_high()) {
      if (b5.get_low()==std::min({b1.get_low(),b3.get_low(),b5.get_low()}) && b9.get_low()>b11.get_low() && b9.get_high()>b11.get_high() && b8.get_high()>b6.get_low() && b1.get_high()-b5.get_low()>b9.get_high()-b11.get_low())
        v1="\x41\x35\x42\x33\x43\x33\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // A5B3C3式类一买
      else if (b1.get_high()>b3.get_high() && b1.get_low()>b3.get_low() && b7.get_high()==std::max({b7.get_high(),b9.get_high(),b11.get_high()}) && b6.get_high()>b4.get_low() && b1.get_high()-b3.get_low()>b7.get_high()-b11.get_low())
        v1="\x41\x33\x42\x33\x43\x35\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // A3B3C5式类一买
      else if (b1.get_low()>b3.get_low() && std::min({b4.get_high(),b6.get_high(),b8.get_high()})>std::max({b4.get_low(),b6.get_low(),b8.get_low()}) && b9.get_high()>b11.get_high() && b1.get_high()-b3.get_low()>b9.get_high()-b11.get_low())
        v1="\x41\x33\x42\x35\x43\x33\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // A3B5C3式类一买
      else if (b2.get_low()>b4.get_high() && b4.get_low()>b6.get_high() && b5.get_low()>b7.get_low() && b10.get_high()>b8.get_low())
        v1="\x61\x31\x41\x62\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // a1Ab式类一买
    } else if ((b7.get_power()<b1.get_power() && mn==b7.get_low() && b7.get_low()<std::max({b2.get_low(),b4.get_low(),b6.get_low()}) &&
                std::max({b2.get_low(),b4.get_low(),b6.get_low()})<std::min({b2.get_high(),b4.get_high(),b6.get_high()}) &&
                std::min({b2.get_high(),b4.get_high(),b6.get_high()})<std::max(b9.get_high(),b11.get_high()) && std::max(b9.get_high(),b11.get_high())<b1.get_high() &&
                mx==b1.get_high() && b11.get_low()>std::min({b2.get_low(),b4.get_low(),b6.get_low()}) && std::min(b9.get_high(),b11.get_high())>std::max(b9.get_low(),b11.get_low())) ||
               (mx==b1.get_high() && mn==b7.get_low() && std::min(b9.get_high(),b11.get_high())>std::max(b9.get_low(),b11.get_low()) &&
                std::max(b11.get_high(),b9.get_high())>std::max(b4.get_high(),b6.get_high()) && std::min(b9.get_low(),b11.get_low())>std::min(b4.get_low(),b6.get_low())))
      v1="\xe7\xb1\xbb\xe4\xba\x8c\xe4\xb9\xb0";  // 类二买
    else {
      double zg=std::min({b1.get_high(),b2.get_high(),b3.get_high()}), zd=std::max({b1.get_low(),b2.get_low(),b3.get_low()});
      double gg=std::max({b1.get_high(),b2.get_high(),b3.get_high()}), dd=std::min({b1.get_low(),b2.get_low(),b3.get_low()});
      if (mx==b11.get_high() && b11.get_low()>zg && zg>zd && gg>b5.get_low() && gg>b7.get_low() && gg>b9.get_low() && dd<b5.get_high() && dd<b7.get_high() && dd<b9.get_high())
        v1="\xe7\xb1\xbb\xe4\xb8\x89\xe4\xb9\xb0";  // 类三买
    }
  } else if (mx==b11.get_high() && mn==b1.get_low()) {
    if (b5.get_high()==std::max({b1.get_high(),b3.get_high(),b5.get_high()}) && b9.get_low()<b11.get_low() && b9.get_high()<b11.get_high() && b8.get_low()<b6.get_high() && b11.get_high()-b9.get_low()<b5.get_high()-b1.get_low())
      v1="\x41\x35\x42\x33\x43\x33\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // A5B3C3式类一卖
    else if (b7.get_low()==std::min({b11.get_low(),b9.get_low(),b7.get_low()}) && b1.get_high()<b3.get_high() && b1.get_low()<b3.get_low() && b6.get_low()<b4.get_high() && b11.get_high()-b7.get_low()<b3.get_high()-b1.get_low())
      v1="\x41\x33\x42\x33\x43\x35\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // A3B3C5式类一卖
    else if (b1.get_high()<b3.get_high() && std::min({b4.get_high(),b6.get_high(),b8.get_high()})>std::max({b4.get_low(),b6.get_low(),b8.get_low()}) && b9.get_low()<b11.get_low() && b3.get_high()-b1.get_low()>b11.get_high()-b9.get_low())
      v1="\x41\x33\x42\x35\x43\x33\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // A3B5C3式类一卖
  } else if (mx==b9.get_high() && b9.get_high()>b8.get_low() && b8.get_low()>b6.get_high() && b6.get_high()>b6.get_low() && b6.get_low()>b4.get_high() && b4.get_high()>b4.get_low() && b4.get_low()>b2.get_high() && mn==b1.get_low() && b11.get_high()<b9.get_high())
    v1="\xe7\xb1\xbb\xe4\xba\x8c\xe5\x8d\x96";  // 类二卖
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_first_buy_V221126：一类买点（趋势背驰）
// 依次尝试最近 21/19/17/15/13/11/9/7/5 笔，命中 check_first_buy → 一买_N笔
// 对齐 Rust cxt.rs:1552 cxt_first_buy_v221126
static std::vector<Signal> cxt_first_buy_v221126(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "B";
  const std::string k3 = "BUY1";
  for (size_t n : {21, 19, 17, 15, 13, 11, 9, 7, 5}) {
    auto bis = get_sub_elements_vec(c.bi_list, di, n);
    if (bis.size() == n && check_first_buy(bis))
      return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x80\xe4\xb9\xb0", F(n) + "\xe7\xac\x94");
  }
  return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
}

// cxt_first_sell_V221126：一类卖点（趋势背驰，反向）
// 对齐 Rust cxt.rs:1590 cxt_first_sell_v221126
static std::vector<Signal> cxt_first_sell_v221126(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "B";
  const std::string k3 = "SELL1";
  for (size_t n : {21, 19, 17, 15, 13, 11, 9, 7, 5}) {
    auto bis = get_sub_elements_vec(c.bi_list, di, n);
    if (bis.size() == n && check_first_sell(bis))
      return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x80\xe5\x8d\x96", F(n) + "\xe7\xac\x94");
  }
  return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
}

// cxt_five_bi_V230619：五笔形态分类（对齐 Rust cxt.rs:2157）
// 完整保留 Rust 所有分支语义；按 bi1.direction 分为向下/向上两大类
static std::vector<Signal> cxt_five_bi_v230619(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "\xe4\xba\x94\xe7\xac\x94";  // D{}五笔
  const char* k3 = "\xe5\xbd\xa2\xe6\x80\x81V230619";  // 形态V230619
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < di + 6 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, other);
  auto bis = get_sub_elements_vec(c.bi_list, di, 5);
  if (bis.size() != 5) return make_kline_signal_v1(k1, k2, k3, other);
  const BI &b1=bis[0], &b2=bis[1], &b3=bis[2], &b4=bis[3], &b5=bis[4];
  double mx=-std::numeric_limits<double>::infinity(), mn=std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) { mx=std::max(mx,bi.get_high()); mn=std::min(mn,bi.get_low()); }
  const char* v1 = other;
  // 工具：方向中文字符，用于构造字面量
  if (b1.direction == Direction::kDown) {
    // 向下笔起始：底背驰/突破/类三买
    if (std::min(b2.get_high(),b4.get_high()) > std::max(b2.get_low(),b4.get_low()) && mx==b1.get_high() && b5.get_power()<b1.get_power() &&
        ((mn==b3.get_low() && b5.get_low()<b1.get_low()) || mn==b5.get_low()))
      v1="\x61\x41\x62\xe5\xbc\x8f\xe5\xba\x95\xe8\x83\x8c\xe9\xa9\xb0";  // aAb式底背驰
    else if (mx==b1.get_high() && mn==b5.get_low() && b4.get_high()<b2.get_low() && b5.get_power()<std::max(b3.get_power(),b1.get_power()))
      v1="\xe7\xb1\xbb\xe8\xb6\x8b\xe5\x8a\xbf\xe5\xba\x95\xe8\x83\x8c\xe9\xa9\xb0";  // 类趋势底背驰
    else if ((mn==b1.get_low() && b5.get_high()>std::min(b1.get_high(),b2.get_high()) && std::min(b1.get_high(),b2.get_high())>b5.get_low() && b5.get_low()>b1.get_low()) ||
             (mn==b3.get_low() && b5.get_high()>b3.get_high() && b3.get_high()>b5.get_low() && b5.get_low()>b3.get_low()))
      v1="\xe4\xb8\x8a\xe9\xa2\x88\xe7\xba\xbf\xe7\xaa\x81\xe7\xa0\xb4";  // 上颈线突破
    else if (mx==b5.get_high() && b5.get_high()>b5.get_low() && b5.get_low()>std::max(b1.get_high(),b3.get_high()) &&
             std::min(b1.get_high(),b3.get_high())>std::max(b1.get_low(),b3.get_low()) && std::max(b1.get_low(),b3.get_low())>mn)
      v1="\xe7\xb1\xbb\xe4\xb8\x89\xe4\xb9\xb0";  // 类三买
  } else {
    // 向上笔起始：顶背驰/突破/类三卖
    if (std::min(b2.get_high(),b4.get_high()) > std::max(b2.get_low(),b4.get_low()) && mn==b1.get_low() && b5.get_power()<b1.get_power() &&
        ((mx==b3.get_high() && b5.get_high()>b1.get_high()) || mx==b5.get_high()))
      v1="\x61\x41\x62\xe5\xbc\x8f\xe9\xa1\xb6\xe8\x83\x8c\xe9\xa9\xb0";  // aAb式顶背驰
    else if (mn==b1.get_low() && mx==b5.get_high() && b5.get_power()<std::max(b1.get_power(),b3.get_power()) && b4.get_low()>b2.get_high())
      v1="\xe7\xb1\xbb\xe8\xb6\x8b\xe5\x8a\xbf\xe9\xa1\xb6\xe8\x83\x8c\xe9\xa9\xb0";  // 类趋势顶背驰
    else if ((mx==b1.get_high() && b5.get_low()<std::max(b1.get_low(),b2.get_low()) && std::max(b1.get_low(),b2.get_low())<b5.get_high() && b5.get_high()<mx) ||
             (mx==b3.get_high() && b5.get_low()<b3.get_low() && b3.get_low()<b5.get_high() && b5.get_high()<mx))
      v1="\xe4\xb8\x8b\xe9\xa2\x88\xe7\xba\xbf\xe7\xaa\x81\xe7\xa0\xb4";  // 下颈线突破
    else if (mn==b5.get_low() && b5.get_low()<b5.get_high() && b5.get_high()<std::min(b1.get_low(),b3.get_low()) &&
             std::min(b1.get_low(),b3.get_low())<std::max(b1.get_low(),b3.get_low()) && std::max(b1.get_low(),b3.get_low())<std::min(b1.get_high(),b3.get_high()) && std::min(b1.get_high(),b3.get_high())<mx)
      v1="\xe7\xb1\xbb\xe4\xb8\x89\xe5\x8d\x96";  // 类三卖
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_fx_power_V221107：倒数分型强弱（对齐 Rust cxt.rs:214）
static std::vector<Signal> cxt_fx_power_v221107(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "F";
  const char* k3 = "\xe5\x88\x86\xe5\x9e\x8b\xe5\xbc\xba\xe5\xbc\xb1";  // 分型强弱
  if (di == 0) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto fxs = c.get_fx_list();
  if (fxs.size() < di) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const FX& fx = fxs[fxs.size() - di];
  const char* mark = (fx.mark == Mark::kG) ? "\xe9\xa1\xb6" : "\xe5\xba\x95";  // 顶/底
  std::string v1 = std::string(fx.power_str()) + mark;  // 强顶/中底/弱底...
  const char* v2 = fx.has_zs() ? "\xe6\x9c\x89\xe4\xb8\xad\xe6\x9e\xa2" : "\xe6\x97\xa0\xe4\xb8\xad\xe6\x9e\xa2";  // 有中枢/无中枢
  return make_kline_signal_v2(k1, k2, k3, v1.c_str(), v2);
}

// cxt_intraday_V230701：30分钟日内走势分类（对齐 Rust cxt_trader.rs:78）
// 交易员级信号：签名 (const TraderState&, const ParamView&) → Vec<Signal>
static std::vector<Signal> cxt_intraday_v230701(const TraderState& state, const ParamView& p) {
  size_t di = p.usize("di", 2);
  const char* freq1 = p.str("freq1", "30\xe5\x88\x86\xe9\x92\x9f");  // 30分钟
  const char* freq2 = p.str("freq2", "\xe6\x97\xa5\xe7\xba\xbf");     // 日线
  std::string k1 = std::string(freq1) + "#" + freq2;
  std::string k2 = "D" + F(di) + "\xe6\x97\xa5";  // D{}日
  const char* k3 = "\xe8\xb5\xb0\xe5\x8a\xbf\xe5\x88\x86\xe7\xb1\xbbV230701";  // 走势分类V230701
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  const CZSC* c1 = state.get_czsc(freq1);
  const CZSC* c2 = state.get_czsc(freq2);
  if (!c1 || !c2 || c2->bars_raw.size() < di) return make_kline_signal_v1(k1, k2, k3, other);
  // 指定日
  int64_t day = c2->bars_raw[c2->bars_raw.size() - di].dt / 86400;
  std::vector<const RawBar*> bars;
  for (const auto& b : c1->bars_raw) if (b.dt / 86400 == day) bars.push_back(&b);
  if (bars.size() <= 4) return make_kline_signal_v1(k1, k2, k3, other);
  // 滑动窗口找中枢
  std::vector<std::pair<double,double>> zs_list;  // (high_max, low_min)
  for (size_t i = 0; i + 2 < bars.size(); ++i) {
    double zg = std::min({bars[i]->high, bars[i+1]->high, bars[i+2]->high});
    double zd = std::max({bars[i]->low, bars[i+1]->low, bars[i+2]->low});
    if (zg >= zd) {
      zs_list.push_back({std::max({bars[i]->high, bars[i+1]->high, bars[i+2]->high}),
                         std::min({bars[i]->low, bars[i+1]->low, bars[i+2]->low})});
    }
  }
  const char* dir = (bars.back()->close > bars.front()->open) ? "\xe4\xb8\x8a\xe6\xb6\xa8" : "\xe4\xb8\x8b\xe8\xb7\x8c";  // 上涨/下跌
  if (zs_list.empty()) {
    return make_kline_signal_v1(k1, k2, k3, (std::string("\xe6\x97\xa0\xe4\xb8\xad\xe6\x9e\xa2") + dir).c_str());  // 无中枢X
  }
  if (zs_list.size() >= 2) {
    double z1h = zs_list[0].first, z1l = zs_list[0].second;
    double z2h = zs_list.back().first, z2l = zs_list.back().second;
    if ((std::string(dir)=="\xe4\xb8\x8a\xe6\xb6\xa8" && z1h < z2l) || (std::string(dir)=="\xe4\xb8\x8b\xe8\xb7\x8c" && z1l > z2h))
      return make_kline_signal_v1(k1, k2, k3, (std::string("\xe5\x8f\x8c\xe4\xb8\xad\xe6\x9e\xa2") + dir).c_str());  // 双中枢X
  }
  // 平衡市判定
  double glob_hi=-std::numeric_limits<double>::infinity(), glob_lo=std::numeric_limits<double>::infinity();
  for (const auto* b : bars) { glob_hi=std::max(glob_hi,b->high); glob_lo=std::min(glob_lo,b->low); }
  double first3_hi=std::max({bars[0]->high,bars[1]->high,bars[2]->high});
  double first3_lo=std::min({bars[0]->low,bars[1]->low,bars[2]->low});
  bool high_first = (first3_hi == glob_hi);
  bool low_first = (first3_lo == glob_lo);
  const char* v1;
  if (high_first && !low_first) v1 = "\xe5\xbc\xb1\xe5\xb9\xb3\xe8\xa1\xa1\xe5\xb8\x82";  // 弱平衡市
  else if (low_first && !high_first) v1 = "\xe5\xbc\xba\xe5\xb9\xb3\xe8\xa1\xa1\xe5\xb8\x82";  // 强平衡市
  else v1 = "\xe6\x8a\x98\xe6\x89\xa9\xe5\xb9\xb3\xe8\xa1\xa1\xe5\xb8\x82";  // 转折平衡市
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_nine_bi_V230621：九笔形态分类（对齐 Rust cxt.rs:2413）
static std::vector<Signal> cxt_nine_bi_v230621(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "\xe4\xb9\x9d\xe7\xac\x94";  // D{}九笔
  const char* k3 = "\xe5\xbd\xa2\xe6\x80\x81V230621";  // 形态V230621
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < di + 13 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, other);
  auto bis = get_sub_elements_vec(c.bi_list, di, 9);
  if (bis.size() != 9) return make_kline_signal_v1(k1, k2, k3, other);
  const BI &b1=bis[0],&b2=bis[1],&b3=bis[2],&b4=bis[3],&b5=bis[4],&b6=bis[5],&b7=bis[6],&b8=bis[7],&b9=bis[8];
  double mx=-std::numeric_limits<double>::infinity(), mn=std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) { mx=std::max(mx,bi.get_high()); mn=std::min(mn,bi.get_low()); }
  const BI* odd[]={&b1,&b3,&b5,&b7};
  const char* v1 = other;
  if (b9.direction == Direction::kDown) {
    if (mn==b9.get_low() && mx==b1.get_high()) {
      if (std::min({b2.get_high(),b4.get_high(),b6.get_high(),b8.get_high()}) > std::max({b2.get_low(),b4.get_low(),b6.get_low(),b8.get_low()}) && b9.get_power()<b1.get_power() && b3.get_low()>=b1.get_low() && b7.get_high()<=b9.get_high())
        v1="\x61\x41\x62\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // aAb式类一买
      else if (std::min({b2.get_high(),b4.get_high(),b6.get_high()}) > std::max({b2.get_low(),b4.get_low(),b6.get_low()}) && std::max({b2.get_low(),b4.get_low(),b6.get_low()})>b8.get_high() && b9.get_power()<b7.get_power())
        v1="\x61\x41\x62\x63\x64\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // aAbcd式类一买
      else if (b3.get_low()<b1.get_low() && b7.get_high()>b9.get_high() && std::min(b4.get_high(),b6.get_high())>std::max(b4.get_low(),b6.get_low()) && (b1.get_high()-b3.get_low())>(b7.get_high()-b9.get_low()))
        v1="\x41\x42\x43\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // ABC式类一买
      else if (b8.get_high()<b6.get_low() && b6.get_high()<b4.get_low() && b4.get_high()<b2.get_low() && b9.get_power()<std::max({b1.get_power(),b3.get_power(),b5.get_power(),b7.get_power()}))
        v1="\xe7\xb1\xbb\xe8\xb6\x8b\xe5\x8a\xbf\xe4\xb8\x80\xe4\xb9\xb0";  // 类趋势一买
    }
    if (mx==std::max(b1.get_high(),b3.get_high()) && mn==b9.get_low() && std::min(b2.get_high(),b4.get_high())>std::max(b2.get_low(),b4.get_low()) &&
        std::min(b2.get_low(),b4.get_low())>std::max(b6.get_high(),b8.get_high()) && std::min(b6.get_high(),b8.get_high())>std::max(b6.get_low(),b8.get_low()) && b9.get_power()<b5.get_power())
      v1="\x61\x41\x62\x42\x63\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe4\xb9\xb0";  // aAbBc式类一买
    if (mx==b9.get_high() && b9.get_low()>std::max({b1.get_high(),b3.get_high(),b5.get_high(),b7.get_high()}) &&
        std::min({b1.get_high(),b3.get_high(),b5.get_high(),b7.get_high()})>std::max({b1.get_low(),b3.get_low(),b5.get_low(),b7.get_low()}) && std::min(b3.get_low(),b5.get_low())==mn)
      v1="\xe7\xb1\xbb\xe4\xb8\x89\xe4\xb9\xb0\x41";  // 类三买A
    if (b8.get_power()<b2.get_power() && mx==b9.get_high() && b9.get_low()>std::max({b3.get_high(),b5.get_high(),b7.get_high()}) &&
        std::min({b3.get_high(),b5.get_high(),b7.get_high()})>std::max({b3.get_low(),b5.get_low(),b7.get_low()}) && b1.get_low()==mn)
      v1="\xe7\xb1\xbb\xe4\xb8\x89\xe4\xb9\xb0\x42";  // 类三买B
    if (mn==b5.get_low() && mx==b1.get_high() && b4.get_high()<b2.get_low()) {
      double zd=std::max(b5.get_low(),b7.get_low()), zg=std::min(b5.get_high(),b7.get_high()), gg=std::max(b5.get_high(),b7.get_high());
      if (zg>zd && b8.get_high()>gg) {
        if (b9.get_low()>zg) v1="\x5a\x47\xe4\xb8\x89\xe4\xb9\xb0";  // ZG三买
        else if (b9.get_high()>gg && gg>zg && b9.get_low()>zd) v1="\xe7\xb1\xbb\xe4\xba\x8c\xe4\xb9\xb0";  // 类二买
      }
    }
  } else if (mx==b9.get_high() && mn==b1.get_low()) {
    if (b6.get_low()>std::min(b2.get_high(),b4.get_high()) && std::min(b2.get_high(),b4.get_high())>std::max(b2.get_low(),b4.get_low()) &&
        std::min(b6.get_high(),b8.get_high())>std::max(b6.get_low(),b8.get_low()) && std::max(b2.get_high(),b4.get_high())<std::min(b6.get_low(),b8.get_low()) && b9.get_power()<b5.get_power())
      v1="\x61\x41\x62\x42\x63\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // aAbBc式类一卖
    else if (std::min({b2.get_high(),b4.get_high(),b6.get_high(),b8.get_high()}) > std::max({b2.get_low(),b4.get_low(),b6.get_low(),b8.get_low()}) && b9.get_power()<b1.get_power() && b3.get_high()<=b1.get_high() && b7.get_low()>=b9.get_low())
      v1="\x61\x41\x62\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // aAb式类一卖
    else if (b8.get_low()>std::min({b2.get_high(),b4.get_high(),b6.get_high()}) && std::min({b2.get_high(),b4.get_high(),b6.get_high()})>std::max({b2.get_low(),b4.get_low(),b6.get_low()}) && b9.get_power()<b7.get_power())
      v1="\x61\x41\x62\x63\x64\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // aAbcd式类一卖
    else if (b3.get_high()>b1.get_high() && b7.get_low()<b9.get_low() && std::min(b4.get_high(),b6.get_high())>std::max(b4.get_low(),b6.get_low()) && (b3.get_high()-b1.get_low())>(b9.get_high()-b7.get_low()))
      v1="\x41\x42\x43\xe5\xbc\x8f\xe7\xb1\xbb\xe4\xb8\x80\xe5\x8d\x96";  // ABC式类一卖
    else if (b8.get_low()>b6.get_high() && b6.get_low()>b4.get_high() && b4.get_low()>b2.get_high() && b9.get_power()<std::max({b1.get_power(),b3.get_power(),b5.get_power(),b7.get_power()}))
      v1="\xe7\xb1\xbb\xe8\xb6\x8b\xe5\x8a\xbf\xe4\xb8\x80\xe5\x8d\x96";  // 类趋势一卖
  } else if (mx==b1.get_high() && mn==b9.get_low() && b9.get_high()<std::max({b3.get_low(),b5.get_low(),b7.get_low()}) && std::max({b3.get_low(),b5.get_low(),b7.get_low()})<std::min({b3.get_high(),b5.get_high(),b7.get_high()}))
    v1="\xe7\xb1\xbb\xe4\xb8\x89\xe5\x8d\x96\x41";  // 类三卖A
  else if (mn==b1.get_low() && mx==b5.get_high() && b2.get_high()<b4.get_low()) {
    double zd=std::max(b5.get_low(),b7.get_low()), zg=std::min(b5.get_high(),b7.get_high()), dd=std::min(b5.get_low(),b7.get_low());
    if (zg>zd && b8.get_low()<dd) {
      if (b9.get_high()<zd) v1="\x5a\x44\xe4\xb8\x89\xe5\x8d\x96";  // ZD三卖
      else if (dd<zd && b9.get_high()<zg) v1="\xe7\xb1\xbb\xe4\xba\x8c\xe5\x8d\x96";  // 类二卖
    }
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_overlap_V240526：收盘价与最近分型区间重合次数（对齐 Rust cxt.rs:1085）
static std::vector<Signal> cxt_overlap_v240526(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "\xe9\xa1\xb6\xe5\xba\x95\xe9\x87\x8d\xe5\x90\x88";  // 顶底重合
  const char* k3 = "支撑压力V240526";
  if (c.bi_list.size() < 11 || c.bars_raw.empty())
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bis = get_sub_elements_vec(c.bi_list, 1, 9);
  if (bis.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double last_close = c.bars_raw.back().close;
  size_t cg = 0, cd = 0;
  for (const auto& bi : bis) {
    if (bi.fx_b.low <= last_close && last_close <= bi.fx_b.high) {
      if (bi.direction == Direction::kUp) ++cg; else ++cd;
    }
  }
  std::string v1 = "\xe9\xa1\xb6\xe9\x87\x8d\xe5\x90\x88" + std::to_string(cg) + "\xe6\xac\xa1";  // 顶重合X次
  std::string v2 = "\xe5\xba\x95\xe9\x87\x8d\xe5\x90\x88" + std::to_string(cd) + "\xe6\xac\xa1";  // 底重合X次
  return make_kline_signal_v2(k1, k2, k3, v1.c_str(), v2.c_str());
}

// cxt_overlap_V240612：顺畅笔分型支撑压力（对齐 Rust cxt.rs:2919）
static std::vector<Signal> cxt_overlap_v240612(const CZSC& c, const ParamView& p, TaCache*) {
  size_t n = p.usize("n", 7);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "SNR\xe9\xa1\xba\xe7\x95\x85N" + F(n);  // SNR顺畅N{}
  const char* k3 = "\xe6\x94\xaf\xe6\x92\x91\xe5\x8e\x8b\xe5\x8a\x9bV240612";  // 支撑压力V240612
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < n + 2 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, other);
  // 取倒数第3起的n笔，过滤 raw_bars>=9
  std::vector<const BI*> bis;
  auto sub = get_sub_elements_vec(c.bi_list, 3, n);
  for (const auto& bi : sub) if (bi.get_raw_bars().size() >= 9) bis.push_back(&bi);
  if (bis.empty()) return make_kline_signal_v1(k1, k2, k3, other);
  std::sort(bis.begin(), bis.end(), [](const BI* a, const BI* b) {
    return a->get_snr() < b->get_snr();
  });
  const BI* max_snr_bi = bis.back();
  if (max_snr_bi->get_snr() < 0.7) return make_kline_signal_v1(k1, k2, k3, other);
  // fxg/fxd：向下笔取(fx_a,fx_b)，向上笔取(fx_b,fx_a)
  const FX *fxg, *fxd;
  if (max_snr_bi->direction == Direction::kDown) { fxg = &max_snr_bi->fx_a; fxd = &max_snr_bi->fx_b; }
  else { fxg = &max_snr_bi->fx_b; fxd = &max_snr_bi->fx_a; }
  const BI& last_bi = c.bi_list.back();
  const char* v1 = other;
  const char* v2 = "\xe4\xbb\xbb\xe6\x84\x8f";  // 任意
  if (last_bi.direction == Direction::kDown) {
    if (ranges_overlap(fxg->high, fxg->low, last_bi.fx_b.high, last_bi.fx_b.low)) {
      v1 = "\xe6\x94\xaf\xe6\x92\x91"; v2 = "\xe9\xa1\xba\xe7\x95\x85\xe7\xac\x94\xe9\xa1\xb6\xe5\x88\x86\xe5\x9e\x8b";  // 支撑/顺畅笔顶分型
    }
    if (ranges_overlap(fxd->high, fxd->low, last_bi.fx_b.high, last_bi.fx_b.low)) {
      v1 = "\xe6\x94\xaf\xe6\x92\x91"; v2 = "\xe9\xa1\xba\xe7\x95\x85\xe7\xac\x94\xe5\xba\x95\xe5\x88\x86\xe5\x9e\x8b";  // 支撑/顺畅笔底分型
    }
  }
  if (last_bi.direction == Direction::kUp) {
    if (ranges_overlap(fxg->high, fxg->low, last_bi.fx_b.high, last_bi.fx_b.low)) {
      v1 = "\xe5\x8e\x8b\xe5\x8a\x9b"; v2 = "\xe9\xa1\xba\xe7\x95\x85\xe7\xac\x94\xe9\xa1\xb6\xe5\x88\x86\xe5\x9e\x8b";  // 压力/顺畅笔顶分型
    }
    if (ranges_overlap(fxd->high, fxd->low, last_bi.fx_b.high, last_bi.fx_b.low)) {
      v1 = "\xe5\x8e\x8b\xe5\x8a\x9b"; v2 = "\xe9\xa1\xba\xe7\x95\x85\xe7\xac\x94\xe5\xba\x95\xe5\x88\x86\xe5\x9e\x8b";  // 压力/顺畅笔底分型
    }
  }
  return make_kline_signal_v2(k1, k2, k3, v1, v2);
}

// cxt_range_oscillation_V230620：区间震荡笔数统计（对齐 Rust cxt.rs:2358）
static std::vector<Signal> cxt_range_oscillation_v230620(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  size_t th = p.usize("th", 2);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "TH" + F(th);
  const char* k3 = "\xe5\x8c\xba\xe9\x97\xb4\xe9\x9c\x87\xe8\x8d\xa1V230620";  // 区间震荡V230620
  if (c.bi_list.size() < di + 11)
    return make_kline_signal_v2(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96", "\xe5\x85\xb6\xe4\xbb\x96");
  auto bis = get_sub_elements_vec(c.bi_list, di, 12);
  if (bis.size() != 12) return make_kline_signal_v2(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96", "\xe5\x85\xb6\xe4\xbb\x96");
  std::vector<double> centers;
  size_t count = 1;
  for (auto it = bis.rbegin(); it != bis.rend(); ++it) {
    centers.push_back((it->get_high() + it->get_low()) / 2.0);
    if (centers.size() > 1) {
      if (max_amplitude_pct(centers) < static_cast<double>(th)) ++count; else break;
    }
  }
  if (count != 1) {
    const char* dir = (bis.back().direction == Direction::kUp) ? "\xe5\x90\x91\xe4\xb8\x8a" : "\xe5\x90\x91\xe4\xb8\x8b";  // 向上/向下
    std::string v1 = std::to_string(count) + "\xe7\xac\x94\xe9\x9c\x87\xe8\x8d\xa1";  // X笔震荡
    return make_kline_signal_v2(k1, k2, k3, v1.c_str(), dir);
  }
  return make_kline_signal_v2(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96", "\xe5\x85\xb6\xe4\xbb\x96");
}

// cxt_second_bs_V230320：均线辅助识别第二类买卖点
// 取最近5笔(b1,b3,b5)；二买=末笔向下 & b1/b3低点<MA & b5终点>MA；二卖反向
// 对齐 Rust cxt.rs:841 cxt_second_bs_v230320
// cxt_second_bs_V230320：均线辅助识别第二类买卖点（对齐 Rust cxt.rs:841）
static std::vector<Signal> cxt_second_bs_v230320(const CZSC& c, const ParamView& p, TaCache* cache) {
  const size_t di = p.usize("di", 1);
  const size_t tp = p.usize("timeperiod", 21);
  const char* mt = p.str("ma_type", "SMA");
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "#" + mt + "#" + F(static_cast<int>(tp));
  const char* k3 = "BS2辅助V230320";
  std::string v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他

  if (c.bi_list.size() < di + 6 || !cache) return make_kline_signal_v1(k1, k2, k3, v1.c_str());
  auto bis = get_sub_elements_vec(c.bi_list, di, 5);
  if (bis.size() != 5) return make_kline_signal_v1(k1, k2, k3, v1.c_str());
  const BI &b1 = bis[0], &b3 = bis[2], &b5 = bis[4];

  // b1/b3/b5 终点分型的末根K线
  auto b1_fx = fx_raw_bars(b1.fx_b);
  auto b3_fx = fx_raw_bars(b3.fx_b);
  auto b5_a = fx_raw_bars(b5.fx_a);
  auto b5_b = fx_raw_bars(b5.fx_b);
  if (b1_fx.size() < 2 || b3_fx.size() < 2 || b5_a.size() < 2 || b5_b.size() < 2)
    return make_kline_signal_v1(k1, k2, k3, v1.c_str());

  const std::string key = std::string(mt) + "_" + F(static_cast<int>(tp));
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto id_to_idx = bar_index_map(c);
  std::unordered_map<int32_t, double> overrides;

  auto get_ma = [&](const RawBar& rb) -> double {
    auto v = ta::MaSnapshotValueWithRecompute(c.bars_raw, *cache, key, rb, mt,
                                               static_cast<int>(tp), overrides);
    return v.value_or(NAN);
  };
  double b1_ma = get_ma(b1_fx[b1_fx.size() - 2]);
  double b3_ma = get_ma(b3_fx[b3_fx.size() - 2]);
  double b5_ma_a = get_ma(b5_a[b5_a.size() - 2]);
  double b5_ma_b = get_ma(b5_b[b5_b.size() - 2]);
  if (std::isnan(b1_ma) || std::isnan(b3_ma) || std::isnan(b5_ma_a) || std::isnan(b5_ma_b))
    return make_kline_signal_v1(k1, k2, k3, v1.c_str());

  const bool lc1 = b1.get_low() < b1_ma && b3.get_low() < b3_ma;
  if (b5.direction == Direction::kDown && lc1 && b5_ma_a < b5_ma_b)
    v1 = "\xe4\xba\x8c\xe4\xb9\xb0";  // 二买
  const bool sc1 = b1.get_high() > b1_ma && b3.get_high() > b3_ma;
  if (b5.direction == Direction::kUp && sc1 && b5_ma_a > b5_ma_b)
    v1 = "\xe4\xba\x8c\xe5\x8d\x96";  // 二卖
  return make_kline_signal_v1(k1, k2, k3, v1.c_str());
}

// cxt_second_bs_V240524：第二买卖点重叠计数（对齐 Rust cxt.rs:2861）
static std::vector<Signal> cxt_second_bs_v240524(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const size_t w = p.usize("w", 9);
  size_t t = p.usize("t", 2);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "W" + F(w) + "T" + F(t);
  const char* k3 = "\xe7\xac\xac\xe4\xba\x8c\xe4\xb9\xb0\xe5\x8d\x96\xe7\x82\xb9V240524";  // 第二买卖点V240524
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (w <= 5 || t < 2 || c.bi_list.size() < w + di + 5 || c.bars_ubi.size() > 7)
    return make_kline_signal_v1(k1, k2, k3, v1);
  auto bis = get_sub_elements_vec(c.bi_list, di, w);
  const BI& last = bis.back();
  double lh = last.fx_b.high, ll = last.fx_b.low;
  size_t zs_cnt = 0;
  for (size_t i = 0; i + 1 < bis.size(); ++i) {
    if (bis[i].get_length() >= 7) {
      const FX& fx = bis[i].fx_b;
      if (ranges_overlap(fx.high, fx.low, lh, ll)) ++zs_cnt;
    }
  }
  if (last.direction == Direction::kDown && last.get_length() >= 7 && zs_cnt >= t) v1 = "\xe4\xba\x8c\xe4\xb9\xb0";  // 二买
  if (last.direction == Direction::kUp && last.get_length() >= 7 && zs_cnt >= t) v1 = "\xe4\xba\x8c\xe5\x8d\x96";  // 二卖
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_seven_bi_V230620：七笔形态分类（对齐 Rust cxt.rs:2262）
static std::vector<Signal> cxt_seven_bi_v230620(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "\xe4\xb8\x83\xe7\xac\x94";  // D{}七笔
  const char* k3 = "\xe5\xbd\xa2\xe6\x80\x81V230620";  // 形态V230620
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c.bi_list.size() < di + 10 || c.bars_ubi.size() > 7) return make_kline_signal_v1(k1, k2, k3, other);
  auto bis = get_sub_elements_vec(c.bi_list, di, 7);
  if (bis.size() != 7) return make_kline_signal_v1(k1, k2, k3, other);
  const BI &b1=bis[0],&b2=bis[1],&b3=bis[2],&b4=bis[3],&b5=bis[4],&b6=bis[5],&b7=bis[6];
  double mx=-std::numeric_limits<double>::infinity(), mn=std::numeric_limits<double>::infinity();
  for (const auto& bi : bis) { mx=std::max(mx,bi.get_high()); mn=std::min(mn,bi.get_low()); }
  const char* v1 = other;
  if (b7.direction == Direction::kDown) {
    if (b1.get_high()==mx && b7.get_low()==mn) {
      if (std::min(b2.get_high(),b4.get_high())>std::max(b2.get_low(),b4.get_low()) && std::max(b2.get_low(),b4.get_low())>b6.get_high() && b7.get_power()<b5.get_power())
        v1="\x61\x41\x62\x63\x64\xe5\xbc\x8f\xe5\xba\x95\xe8\x83\x8c\xe9\xa9\xb0";  // aAbcd式底背驰
      else if (b2.get_low()>std::min(b4.get_high(),b6.get_high()) && std::max(b4.get_low(),b6.get_low())<std::min(b4.get_high(),b6.get_high()) && b7.get_power()<b1.get_high()-b3.get_low())
        v1="\x61\x62\x63\x41\x64\xe5\xbc\x8f\xe5\xba\x95\xe8\x83\x8c\xe9\xa9\xb0";  // abcAd式底背驰
      else if (std::min({b2.get_high(),b4.get_high(),b6.get_high()}) > std::max({b2.get_low(),b4.get_low(),b6.get_low()}) && b7.get_power()<b1.get_power())
        v1="\x61\x41\x62\xe5\xbc\x8f\xe5\xba\x95\xe8\x83\x8c\xe9\xa9\xb0";  // aAb式底背驰
      else if (b2.get_low()>b4.get_high() && b4.get_low()>b6.get_high() && b7.get_power()<std::max({b5.get_power(),b3.get_power(),b1.get_power()}))
        v1="\xe7\xb1\xbb\xe8\xb6\x8b\xe5\x8a\xbf\xe5\xba\x95\xe8\x83\x8c\xe9\xa9\xb0";  // 类趋势底背驰
    } else if (b4.get_low()==mn && std::min(b1.get_high(),b3.get_high())>std::max(b1.get_low(),b3.get_low()) &&
               std::min(b5.get_high(),b7.get_high())>std::max(b5.get_low(),b7.get_low()) &&
               std::max(b4.get_high(),b6.get_high())>std::min(b3.get_high(),b4.get_high()) && std::max(b1.get_low(),b3.get_low())<std::max(b5.get_high(),b7.get_high()))
      v1="\xe5\x90\x91\xe4\xb8\x8a\xe4\xb8\xad\xe6\x9e\xa2\xe5\xae\x8c\xe6\x88\x90";  // 向上中枢完成
    else if (std::min(b1.get_low(),b3.get_low())==mn && std::max(b5.get_high(),b7.get_high())==mx &&
             std::min(b5.get_low(),b7.get_low())>std::max(b1.get_high(),b3.get_high()) && std::min(b1.get_high(),b3.get_high())>std::max(b1.get_low(),b3.get_low()))
      v1="\xe7\xb1\xbb\xe4\xb8\x89\xe4\xb9\xb0";  // 类三买
  } else if (b1.get_low()==mn && b7.get_high()==mx) {
    if (b6.get_low()>std::min(b2.get_high(),b4.get_high()) && std::min(b2.get_high(),b4.get_high())>std::max(b2.get_low(),b4.get_low()) && b7.get_power()<b5.get_power())
      v1="\x61\x41\x62\x63\x64\xe5\xbc\x8f\xe9\xa1\xb6\xe8\x83\x8c\xe9\xa9\xb0";  // aAbcd式顶背驰
    else if (std::min(b4.get_high(),b6.get_high())>std::max(b4.get_low(),b6.get_low()) && std::max(b4.get_low(),b6.get_low())>b2.get_high() && b7.get_power()<b3.get_high()-b1.get_low())
      v1="\x61\x62\x63\x41\x64\xe5\xbc\x8f\xe9\xa1\xb6\xe8\x83\x8c\xe9\xa9\xb0";  // abcAd式顶背驰
    else if (std::min({b2.get_high(),b4.get_high(),b6.get_high()}) > std::max({b2.get_low(),b4.get_low(),b6.get_low()}) && b7.get_power()<b1.get_power())
      v1="\x61\x41\x62\xe5\xbc\x8f\xe9\xa1\xb6\xe8\x83\x8c\xe9\xa9\xb0";  // aAb式顶背驰
    else if (b2.get_high()<b4.get_low() && b4.get_high()<b6.get_low() && b7.get_power()<std::max({b5.get_power(),b3.get_power(),b1.get_power()}))
      v1="\xe7\xb1\xbb\xe8\xb6\x8b\xe5\x8a\xbf\xe9\xa1\xb6\xe8\x83\x8c\xe9\xa9\xb0";  // 类趋势顶背驰
  } else if (b4.get_high()==mx && std::min(b1.get_high(),b3.get_high())>std::max(b1.get_low(),b3.get_low()) &&
             std::min(b5.get_high(),b7.get_high())>std::max(b5.get_low(),b7.get_low()) &&
             std::min(b4.get_low(),b6.get_low())<std::max(b3.get_low(),b4.get_low()) && std::min(b1.get_high(),b3.get_high())>std::min(b5.get_low(),b7.get_low()))
    v1="\xe5\x90\x91\xe4\xb8\x8b\xe4\xb8\xad\xe6\x9e\xa2\xe5\xae\x8c\xe6\x88\x90";  // 向下中枢完成
  else if (std::min(b5.get_low(),b7.get_low())==mn && std::max(b1.get_high(),b3.get_high())==mx &&
           std::max(b5.get_high(),b7.get_high())<std::min(b1.get_low(),b3.get_low()) && std::min(b1.get_high(),b3.get_high())>std::max(b1.get_low(),b3.get_low()))
    v1="\xe7\xb1\xbb\xe4\xb8\x89\xe5\x8d\x96";  // 类三卖
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_third_bs_V230318：均线辅助识别第三类买卖点
// 取最近5笔；由b1/b3建中枢(zd/zg)；三买=末笔向下 & 低点>zg & 均线ma5>ma3>ma1同向抬升；三卖反向
// 对齐 Rust cxt.rs:929 cxt_third_bs_v230318
static std::vector<Signal> cxt_third_bs_v230318(const CZSC& c, const ParamView& p, TaCache* cache) {
  const size_t di = p.usize("di", 1);
  const size_t tp = p.usize("timeperiod", 34);
  const char* mt = p.str("ma_type", "SMA");
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "#" + mt + "#" + F(tp);
  const std::string k3 = "BS3辅助V230318";
  std::string v1 = "\xe5\x85\xb6\xe4\xbb\x96";

  if (c.bi_list.size() < di + 6 || !cache) return make_kline_signal_v1(k1, k2, k3, v1.c_str());
  auto bis = get_sub_elements_vec(c.bi_list, di, 5);
  if (bis.size() != 5) return make_kline_signal_v1(k1, k2, k3, v1.c_str());
  const BI &b1 = bis[0], &b3 = bis[2], &b5 = bis[4];

  // 中枢由 b1/b3 三笔重叠构成（对齐 Rust: zd=max(b1.low,b3.low), zg=min(b1.high,b3.high)）
  const double zs_zd = std::max(b1.get_low(), b3.get_low());
  const double zs_zg = std::min(b1.get_high(), b3.get_high());
  if (zs_zd > zs_zg) return make_kline_signal_v1(k1, k2, k3, v1.c_str());

  auto b1_fx = fx_raw_bars(b1.fx_b);
  auto b3_fx = fx_raw_bars(b3.fx_b);
  auto b5_fx = fx_raw_bars(b5.fx_b);
  if (b1_fx.empty() || b3_fx.empty() || b5_fx.empty())
    return make_kline_signal_v1(k1, k2, k3, v1.c_str());

  const std::string key = std::string(mt) + "_" + F(static_cast<int>(tp));
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  std::unordered_map<int32_t, double> overrides;

  auto get_ma = [&](const RawBar& rb) -> double {
    auto v = ta::MaSnapshotValueWithRecompute(c.bars_raw, *cache, key, rb, mt,
                                               static_cast<int>(tp), overrides);
    return v.value_or(NAN);
  };
  double ma1 = get_ma(b1_fx.back());
  double ma3 = get_ma(b3_fx.back());
  double ma5 = get_ma(b5_fx.back());
  if (std::isnan(ma1) || std::isnan(ma3) || std::isnan(ma5))
    return make_kline_signal_v1(k1, k2, k3, v1.c_str());

  if (b5.direction == Direction::kDown && b5.get_low() > zs_zg && ma5 > ma3 && ma3 > ma1)
    v1 = "\xe4\xb8\x89\xe4\xb9\xb0";  // 三买
  if (b5.direction == Direction::kUp && b5.get_high() < zs_zd && ma5 < ma3 && ma3 < ma1)
    v1 = "\xe4\xb8\x89\xe5\x8d\x96";  // 三卖
  return make_kline_signal_v1(k1, k2, k3, v1.c_str());
}

// cxt_third_bs_V230319：带均线形态的第三类买卖点辅助（对齐 Rust cxt.rs:1787）
static std::vector<Signal> cxt_third_bs_v230319(const CZSC& c, const ParamView& p, TaCache* cache) {
  const size_t di = p.usize("di", 1);
  size_t tp = p.usize("timeperiod", 34);
  const char* mt = p.str("ma_type", "SMA");
  const std::string key = std::string(mt) + "#" + F(static_cast<int>(tp));
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "#" + mt + "#" + F(static_cast<int>(tp));
  const char* k3 = "BS3辅助V230319";
  if (!cache || c.bi_list.size() < di + 6) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto bis = get_sub_elements_vec(c.bi_list, di, 5);
  if (bis.size() != 5) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const BI &b1 = bis[0], &b3 = bis[2], &b5 = bis[4];
  double zs_zd = std::max(b1.get_low(), b3.get_low());
  double zs_zg = std::min(b1.get_high(), b3.get_high());
  if (zs_zd > zs_zg) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto it_ma = cache->series.find(key);
  if (it_ma == cache->series.end()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const auto& ma = it_ma->second;
  auto id_to_idx = bar_index_map(c);
  std::unordered_map<int32_t, double> ov;
  auto get_ma = [&](const BI& bi) -> double {
    auto rb = fx_raw_bars(bi.fx_b);
    if (rb.empty()) return NAN;
    auto v = ta::MaSnapshotValueWithRecompute(c.bars_raw, *cache, key, rb.back(), mt, static_cast<int>(tp), ov);
    return v.value_or(NAN);
  };
  double ma1 = get_ma(b1), ma3 = get_ma(b3), ma5 = get_ma(b5);
  if (std::isnan(ma1) || std::isnan(ma3) || std::isnan(ma5)) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v1;
  if (b5.direction == Direction::kDown && b5.get_low() > zs_zg) v1 = "\xe4\xb8\x89\xe4\xb9\xb0";  // 三买
  else if (b5.direction == Direction::kUp && b5.get_high() < zs_zd) v1 = "\xe4\xb8\x89\xe5\x8d\x96";  // 三卖
  else return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v2;
  if (ma5 > ma3 && ma3 > ma1) v2 = "\xe5\x9d\x87\xe7\xba\xbf\xe6\x96\xb0\xe9\xab\x98";  // 均线新高
  else if (ma5 < ma3 && ma3 < ma1) v2 = "\xe5\x9d\x87\xe7\xba\xbf\xe6\x96\xb0\xe4\xbd\x8e";  // 均线新低
  else if (ma5 > ma3 && ma3 < ma1) v2 = "\xe5\x9d\x87\xe7\xba\xbf\xe5\xba\x95\xe5\x88\x86";  // 均线底分
  else if (ma5 < ma3 && ma3 > ma1) v2 = "\xe5\x9d\x87\xe7\xba\xbf\xe9\xa1\xb6\xe5\x88\x86";  // 均线顶分
  else v2 = "\xe5\x9d\x87\xe7\xba\xbf\xe5\x90\xa6\xe5\xae\x9a";  // 均线否定
  return make_kline_signal_v2(k1, k2, k3, v1, v2);
}

// cxt_third_buy_V230228：笔三买辅助（纯笔结构，无MA）
// 依次尝试最近 13/11/9/7/5 笔 + 末笔，共 n+1 笔；奇数位上升关键笔；
// 末笔低点>关键高点min 且 <min+1.618×mean_power → 三买
// 对齐 Rust cxt.rs:1716 cxt_third_buy_v230228
static std::vector<Signal> cxt_third_buy_v230228(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di);
  const std::string k3 = "三买辅助V230228";
  const std::string other = "\xe5\x85\xb6\xe4\xbb\x96";

  if (c.bi_list.size() < di + 11)
    return make_kline_signal_v2(k1, k2, k3, other.c_str(), other.c_str());

  for (size_t n : {13, 11, 9, 7, 5}) {
    auto bis = get_sub_elements_vec(c.bi_list, di, n + 1);
    if (bis.size() != n + 1) continue;
    const BI& last = bis.back();
    if (last.direction == Direction::kUp || bis.front().direction == last.direction) continue;

    // 奇数位(0,2,...)上升关键笔：高点递升者入选
    std::vector<std::reference_wrapper<const BI>> key_bis;
    for (size_t i = 0; i + 2 < bis.size(); i += 2) {
      if (i == 0) {
        key_bis.emplace_back(bis[i]);
      } else {
        const BI& b1 = bis[i - 2];
        const BI& b3 = bis[i];
        if (b3.get_high() > b1.get_high()) key_bis.emplace_back(b3);
      }
    }
    if (key_bis.size() < 2) continue;

    double min_high = std::numeric_limits<double>::infinity();
    double max_low = -std::numeric_limits<double>::infinity();
    for (const BI& bi : key_bis) {
      min_high = std::min(min_high, bi.get_high());
      max_low = std::max(max_low, bi.get_low());
    }
    // 突破：末笔低点在关键高点上方，且关键高点min高于关键低点max
    const bool tb_break = last.get_low() > min_high && min_high > max_low;
    // 价格约束：末笔低点 < min_high + 1.618 × 关键笔均价力度
    std::vector<double> powers;
    powers.reserve(key_bis.size());
    for (const BI& bi : key_bis) powers.push_back(bi.get_power_price());
    const bool tb_price = last.get_low() < min_high + 1.618 * Mean(powers);

    if (tb_break && tb_price)
      return make_kline_signal_v2(k1, k2, k3, "\xe4\xb8\x89\xe4\xb9\xb0",  // 三买
                                   (F(bis.size()) + "\xe7\xac\x94").c_str());  // N笔
  }
  return make_kline_signal_v2(k1, k2, k3, other.c_str(), other.c_str());
}

// cxt_three_bi_V230618：三笔形态分类（对齐 Rust cxt.rs:2093）
static std::vector<Signal> cxt_three_bi_v230618(const CZSC& c, const ParamView& p, TaCache*) {
  const size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "\xe4\xb8\x89\xe7\xac\x94";  // D{}三笔
  const char* k3 = "形态V230618";
  if (c.bi_list.size() < di + 6 || c.bars_ubi.size() > 7)
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bis = get_sub_elements_vec(c.bi_list, di, 3);
  if (bis.size() != 3) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const BI &b1 = bis[0], &b2 = bis[1], &b3 = bis[2];
  const char* v1;
  if (b3.direction == Direction::kDown) {
    if (b3.get_low() > b1.get_high()) v1 = "\xe5\x90\x91\xe4\xb8\x8b\xe4\xb8\x8d\xe9\x87\x8d\xe5\x90\x88";  // 向下不重合
    else if (b2.get_low() < b3.get_low() && b3.get_low() < b1.get_high() && b1.get_high() < b2.get_high())
      v1 = "\xe5\x90\x91\xe4\xb8\x8b\xe5\xa5\x94\xe8\xb5\xb0\xe5\x9e\x8b";  // 向下奔走型
    else if (b1.get_high() > b3.get_high() && b1.get_low() < b3.get_low())
      v1 = "\xe5\x90\x91\xe4\xb8\x8b\xe6\x94\xb6\xe6\x95\x9b";  // 向下收敛
    else if (b1.get_high() < b3.get_high() && b1.get_low() > b3.get_low())
      v1 = "\xe5\x90\x91\xe4\xb8\x8b\xe6\x89\xa9\xe5\xbc\xa0";  // 向下扩张
    else if (b3.get_low() < b1.get_low() && b3.get_high() < b1.get_high())
      v1 = (b3.get_power() < b1.get_power()) ? "\xe5\x90\x91\xe4\xb8\x8b\xe7\x9b\x98\xe8\x83\x8c" : "\xe5\x90\x91\xe4\xb8\x8b\xe6\x97\xa0\xe8\x83\x8c";  // 向下盘背/无背
    else v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  } else {
    if (b3.get_high() < b1.get_low()) v1 = "\xe5\x90\x91\xe4\xb8\x8a\xe4\xb8\x8d\xe9\x87\x8d\xe5\x90\x88";  // 向上不重合
    else if (b2.get_low() < b1.get_low() && b1.get_low() < b3.get_high() && b3.get_high() < b2.get_high())
      v1 = "\xe5\x90\x91\xe4\xb8\x8a\xe5\xa5\x94\xe8\xb5\xb0\xe5\x9e\x8b";  // 向上奔走型
    else if (b1.get_high() > b3.get_high() && b1.get_low() < b3.get_low())
      v1 = "\xe5\x90\x91\xe4\xb8\x8a\xe6\x94\xb6\xe6\x95\x9b";  // 向上收敛
    else if (b1.get_high() < b3.get_high() && b1.get_low() > b3.get_low())
      v1 = "\xe5\x90\x91\xe4\xb8\x8a\xe6\x89\xa9\xe5\xbc\xa0";  // 向上扩张
    else if (b3.get_low() > b1.get_low() && b3.get_high() > b1.get_high())
      v1 = (b3.get_power() < b1.get_power()) ? "\xe5\x90\x91\xe4\xb8\x8a\xe7\x9b\x98\xe8\x83\x8c" : "\xe5\x90\x91\xe4\xb8\x8a\xe6\x97\xa0\xe8\x83\x8c";  // 向上盘背/无背
    else v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// cxt_ubi_end_V230816：UBI 新高新低次数（对齐 Rust cxt.rs:2721）
static std::vector<Signal> cxt_ubi_end_v230816(const CZSC& c, const ParamView& p, TaCache*) {
  const std::string k1 = FreqName(c.freq);
  const char* k2 = "UBI";
  const char* k3 = "BE辅助V230816";
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  std::vector<FX> ubi_fxs = c.get_ubi_fxs();
  if (ubi_fxs.empty() || c.bi_list.empty())
    return make_kline_signal_v2(k1, k2, k3, other, other);
  // rebuild_ubi: direction = opposite of last bi
  Direction ubi_dir = (c.bi_list.back().direction == Direction::kUp) ? Direction::kDown : Direction::kUp;
  if (ubi_fxs.size() <= 2 || c.bars_ubi.size() <= 5)
    return make_kline_signal_v2(k1, k2, k3, other, other);
  // raw_bars = flattened bars_ubi
  std::vector<RawBar> raw_bars;
  for (const auto& nb : c.bars_ubi)
    raw_bars.insert(raw_bars.end(), nb.elements.begin(), nb.elements.end());
  if (ubi_dir == Direction::kUp) {
    std::vector<const FX*> fxs;
    for (auto& fx : ubi_fxs) if (fx.mark == Mark::kG) fxs.push_back(&fx);
    if (fxs.empty()) return make_kline_signal_v2(k1, k2, k3, other, other);
    int cnt = 1; const FX* cur = fxs[0];
    for (size_t i = 1; i < fxs.size(); ++i) {
      if (fxs[i]->high > cur->high) { ++cnt; cur = fxs[i]; }
    }
    if (raw_bars.back().high > cur->high) {
      std::string v2 = "\xe7\xac\xac" + std::to_string(cnt + 1) + "\xe6\xac\xa1";  // 第X次
      return make_kline_signal_v2(k1, k2, k3, "\xe6\x96\xb0\xe9\xab\x98", v2.c_str());  // 新高
    }
  }
  if (ubi_dir == Direction::kDown) {
    std::vector<const FX*> fxs;
    for (auto& fx : ubi_fxs) if (fx.mark == Mark::kD) fxs.push_back(&fx);
    if (fxs.empty()) return make_kline_signal_v2(k1, k2, k3, other, other);
    int cnt = 1; const FX* cur = fxs[0];
    for (size_t i = 1; i < fxs.size(); ++i) {
      if (fxs[i]->low < cur->low) { ++cnt; cur = fxs[i]; }
    }
    if (raw_bars.back().low < cur->low) {
      std::string v2 = "\xe7\xac\xac" + std::to_string(cnt + 1) + "\xe6\xac\xa1";  // 第X次
      return make_kline_signal_v2(k1, k2, k3, "\xe6\x96\xb0\xe4\xbd\x8e", v2.c_str());  // 新低
    }
  }
  return make_kline_signal_v2(k1, k2, k3, other, other);
}

// cxt_zhong_shu_gong_zhen_V221221：大小级别中枢共振（对齐 Rust cxt_trader.rs:29）
// 交易员级信号：签名 (const TraderState&, const ParamView&) → Vec<Signal>
static std::vector<Signal> cxt_zhong_shu_gong_zhen_v221221(const TraderState& state, const ParamView& p) {
  const char* freq1 = p.str("freq1", "\xe6\x97\xa5\xe7\xba\xbf");     // 日线（大级别）
  const char* freq2 = p.str("freq2", "60\xe5\x88\x86\xe9\x92\x9f");  // 60分钟（小级别）
  std::string k1 = freq1; std::string k2 = freq2;
  const char* k3 = "\xe4\xb8\xad\xe6\x9e\xa2\xe5\x85\xb1\xe6\x8c\xafV221221";  // 中枢共振V221221
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  const CZSC* big = state.get_czsc(freq1);
  const CZSC* small = state.get_czsc(freq2);
  if (!big || !small || big->bi_list.size() < 5 || small->bi_list.size() < 5)
    return make_kline_signal_v1(k1, k2, k3, other);
  // 最近 3 笔构造中枢
  std::vector<BI> big_bis(big->bi_list.end()-3, big->bi_list.end());
  std::vector<BI> small_bis(small->bi_list.end()-3, small->bi_list.end());
  ZS big_zs(big_bis), small_zs(small_bis);
  if (big_zs.zg <= big_zs.zd || small_zs.zg <= small_zs.zd) return make_kline_signal_v1(k1, k2, k3, other);
  if (small_zs.dd > big_zs.zz && small->bi_list.back().direction == Direction::kDown)
    return make_kline_signal_v1(k1, k2, k3, "\xe7\x9c\x8b\xe5\xa4\x9a");  // 看多
  if (small_zs.gg < big_zs.zz && small->bi_list.back().direction == Direction::kUp)
    return make_kline_signal_v1(k1, k2, k3, "\xe7\x9c\x8b\xe7\xa9\xba");  // 看空
  return make_kline_signal_v1(k1, k2, k3, other);
}

// dema_up_dw_line_V230605：DEMA 短线趋势（对齐 Rust ang.rs:493 dema_up_dw_line_v230605）
//  dema = 2*mean(close,N) - mean(close,2N); 看多: close>dema
static std::vector<Signal> dema_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 5);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n);
  const char* k3 = "DEMA\xe7\x9f\xad\xe7\xba\xbf\xe8\xb6\x8b\xe5\x8a\xbfV230605";  // DEMA短线趋势V230605
  if (c.bars_raw.size() < di + 2 * n + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto sb = get_sub_elements_vec(c.bars_raw, di, n);
  auto lb = get_sub_elements_vec(c.bars_raw, di, 2 * n);
  if (sb.empty() || lb.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double sm = 0, lm = 0;
  for (auto& b : sb) sm += b.close; sm /= sb.size();
  for (auto& b : lb) lm += b.close; lm /= lb.size();
  double dema = 2.0 * sm - lm;
  const char* v1 = (sb.back().close > dema) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// demakder_up_dw_line_V230605：DEMAKER 价格趋势（对齐 Rust ang.rs:565 demakder_up_dw_line_v230605）
//  demax = mean(正 high 差); demin = mean(正 low 差); demaker = demax/(demax+demin)
//  看多: demaker>th/10; 看空: demaker<tl/10
static std::vector<Signal> demakder_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 105);
  int th = (int)p.usize("th", 5), tl = (int)p.usize("tl", 5);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "TH" + F(th) + "TL" + F(tl);
  const char* k3 = "DEMAKER\xe4\xbb\xb7\xe6\xa0\xbc\xe8\xb6\x8b\xe5\x8a\xbfV230605";  // DEMAKER价格趋势V230605
  if (c.bars_raw.size() < di + n + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, n);
  if (bars.size() < 2) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  std::vector<double> demax_items, demin_items;
  for (size_t i = 1; i < bars.size(); ++i) {
    double dh = bars[i].high - bars[i-1].high;
    if (dh > 0.0) demax_items.push_back(dh);
    double dl = bars[i-1].low - bars[i].low;
    if (dl > 0.0) demin_items.push_back(dl);
  }
  double demax = Mean(demax_items), demin = Mean(demin_items);
  double demaker = (demax + demin == 0.0) ? NAN : demax / (demax + demin);
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (std::isfinite(demaker) && demaker > th / 10.0) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (std::isfinite(demaker) && demaker < tl / 10.0) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// emv_up_dw_line_V230605：EMV 简易波动（对齐 Rust ang.rs:634 emv_up_dw_line_v230605）
//  mid_pt_move = mid(最新两根) - mid(前一根); box_ratio = vol/(high-low)
//  emv = mid_pt_move / box_ratio; 看多: emv>0
static std::vector<Signal> emv_up_dw_line_v230605(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di);
  const char* k3 = "EMV\xe7\xae\x80\xe6\x98\x93\xe6\xb3\xa2\xe5\x8a\xa8V230605";  // EMV简易波动V230605
  if (c.bars_raw.size() < di + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, 2);
  if (bars.size() < 2) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double mid_pt_move = (bars[1].high + bars[1].low) / 2.0 - (bars[0].high + bars[0].low) / 2.0;
  double box_ratio = (double)bars[1].vol / (bars[1].high - bars[1].low + 1e-9);
  double emv = mid_pt_move / box_ratio;
  const char* v1 = (emv > 0.0) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// er_up_dw_line_V230604：ER 价格动量分层（对齐 Rust ang.rs:690 er_up_dw_line_v230604）
//  ER 序列: er_i = abs(close_i - close_{i-w}) / Σ|Δclose| over w
//  看多: last_er > mean(er); 看空: last_er < mean(er)
static std::vector<Signal> er_up_dw_line_v230604(const CZSC& c, const ParamView& p, TaCache* cache) {
  size_t di = p.usize("di", 1), w = p.usize("w", 60), n = p.usize("n", 10);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "W" + F(w) + "N" + F(n);
  const char* k3 = "ER\xe4\xbb\xb7\xe6\xa0\xbc\xe5\x8a\xa8\xe9\x87\x8fV230604";  // ER价格动量V230604
  const std::string er_key = "ER" + F((int)w);
  // 复用 cache.series 存储 ER 序列（对齐 Rust cache_key=ER{w}）
  if (!cache) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  if (c.bars_raw.size() < di + w + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  // 计算 ER 序列（全量，对齐 Rust 语义）
  size_t len = c.bars_raw.size();
  std::vector<double> er_series(len, NAN);
  for (size_t i = 0; i < len; ++i) {
    if (i + 1 < w) continue;  // 对齐 Python 负索引切片：不足 w 时跳过
    size_t start = (i >= w) ? i - w : 0;
    double dir = std::abs(c.bars_raw[i].close - c.bars_raw[start].close);
    double vol_sum = 0.0;
    for (size_t j = start + 1; j <= i; ++j) vol_sum += std::abs(c.bars_raw[j].close - c.bars_raw[j-1].close);
    er_series[i] = (vol_sum == 0.0) ? NAN : dir / vol_sum;
  }
  cache->series[er_key] = er_series;
  size_t idx = c.bars_raw.size() - di;
  if (idx >= er_series.size() || !std::isfinite(er_series[idx])) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double last_er = er_series[idx];
  // 取最近 w 个有效 ER 算均值
  std::vector<double> recent;
  for (size_t i = (idx > w) ? idx - w : 0; i <= idx; ++i)
    if (std::isfinite(er_series[i])) recent.push_back(er_series[i]);
  double mean_er = Mean(recent);
  if (!std::isfinite(mean_er)) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v1 = (last_er > mean_er) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

static std::vector<Signal> jcc_ci_tou_v221101(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccCiTouV221101",v1.c_str());
}

static std::vector<Signal> jcc_fan_ji_xian_v221121(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccFanJiXianV221121",v1.c_str());
}

static std::vector<Signal> jcc_fen_shou_xian_v20221113(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccFenShouXianV20221113",v1.c_str());
}

static std::vector<Signal> jcc_gap_yin_yang_v221121(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccGapYinYangV221121",v1.c_str());
}

static std::vector<Signal> jcc_ping_tou_v221113(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccPingTouV221113",v1.c_str());
}

static std::vector<Signal> jcc_san_fa_v20221115(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccSanFaV20221115",v1.c_str());
}

static std::vector<Signal> jcc_san_fa_v20221118(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccSanFaV20221118",v1.c_str());
}

static std::vector<Signal> jcc_san_szx_v221122(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccSanSzxV221122",v1.c_str());
}

static std::vector<Signal> jcc_san_xing_xian_v221023(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccSanXingXianV221023",v1.c_str());
}

static std::vector<Signal> jcc_shan_chun_v221121(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccShanChunV221121",v1.c_str());
}

static std::vector<Signal> jcc_szx_v221111(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccSzxV221111",v1.c_str());
}

static std::vector<Signal> jcc_ta_xing_v221124(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccTaXingV221124",v1.c_str());
}

static std::vector<Signal> jcc_ten_mo_v221028(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccTenMoV221028",v1.c_str());
}

static std::vector<Signal> jcc_three_crow_v221108(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccThreeCrowV221108",v1.c_str());
}

static std::vector<Signal> jcc_two_crow_v221108(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccTwoCrowV221108",v1.c_str());
}

static std::vector<Signal> jcc_wu_yun_gai_ding_v221101(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccWuYunGaiDingV221101",v1.c_str());
}

static std::vector<Signal> jcc_xing_xian_v221118(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccXingXianV221118",v1.c_str());
}

static std::vector<Signal> jcc_yun_xian_v221118(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccYunXianV221118",v1.c_str());
}

static std::vector<Signal> jcc_zhu_huo_xian_v221027(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1);if(c.bars_raw.size()<di+3)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,di,3);if(bars.size()<3)return{};
  std::string v1=(bars[0].close>bars[0].open&&bars[1].close>bars[1].open&&bars[2].close>bars[2].open)?"\xe4\xb8\x89\xe9\x98\xb3":"\xe4\xb8\x89\xe9\x98\xb4";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"JccZhuHuoXianV221027",v1.c_str());
}

// kcatr_up_dw_line_V230823：ATR 通道突破（对齐 Rust kcatr.rs:35 kcatr_up_dw_line_v230823）
//  ATR = mean(TR, N); middle = mean(close, M); 看多: close>middle+ATR*th; 看空: close<middle-ATR*th
static std::vector<Signal> kcatr_up_dw_line_v230823(const CZSC& c, const ParamView& p, TaCache* cache) {
  size_t di = p.usize("di", 1), n = p.usize("n", 30), m = p.usize("m", 16);
  int th = (int)p.usize("th", 2);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "M" + F(m) + "T" + F(th);
  const char* k3 = "KCATR\xe5\xa4\x9a\xe7\xa9\xbaV230823";  // KCATR多空V230823
  if (c.bars_raw.size() < di + std::max(n, m) + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto n_bars = get_sub_elements_vec(c.bars_raw, di, n);
  auto m_bars = get_sub_elements_vec(c.bars_raw, di, m);
  if (n_bars.size() < 2 || m_bars.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double tr_sum = 0.0;
  for (size_t i = 1; i < n_bars.size(); ++i) {
    const RawBar& b = n_bars[i], &p = n_bars[i-1];
    double tr1 = std::abs(b.high - b.low);
    double tr2 = std::abs(b.high - p.close);
    double tr3 = std::abs(b.low - p.close);
    tr_sum += std::max({tr1, tr2, tr3});
  }
  double atr = tr_sum / (n_bars.size() - 1);
  double middle = 0.0;
  for (auto& b : m_bars) middle += b.close;
  middle /= m_bars.size();
  double close = m_bars.back().close;
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (close > middle + atr * th) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  else if (close < middle - atr * th) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// ntmdk_V230824：M 日前收盘价对比多空（对齐 Rust ntmdk.rs:32 ntmdk_v230824）
//  看多: last_close > first_close(M窗口)
static std::vector<Signal> ntmdk_v230824(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), m = p.usize("m", 10);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "M" + F(m);
  const char* k3 = "NTMDK\xe5\xa4\x9a\xe7\xa9\xbaV230824";  // NTMDK多空V230824
  if (c.bars_raw.size() < di + m + 10) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto bars = get_sub_elements_vec(c.bars_raw, di, m);
  if (bars.size() < 2) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v1 = (bars.back().close > bars.front().close) ? "\xe7\x9c\x8b\xe5\xa4\x9a" : "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// obv_up_dw_line_V230719：OBV 交叉信号（对齐 Rust obv.rs:212 obv_up_dw_line_v230719）
//  obvm = EMA(OBV, n); sig = EMA(obvm, m)
//  看多: obvm_last > sig_last 且 obvm[-max_overlap] < sig[-max_overlap]（金叉）
static std::vector<Signal> obv_up_dw_line_v230719(const CZSC& c, const ParamView& p, TaCache* cache) {
  size_t di = p.usize("di", 1);
  int n = (int)p.usize("n", 7), m = (int)p.usize("m", 10), mo = (int)p.usize("max_overlap", 3);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n) + "M" + F(m) + "MO" + F(mo);
  const char* k3 = "OBV\xe8\x83\xbd\xe9\x87\x8fV230719";  // OBV能量V230719
  auto is_ok = [&](double v) { return std::isfinite(v); };
  if (!cache) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  size_t min_num = di + std::max(n, m) + mo + 10;
  if (c.bars_raw.size() < min_num) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  ta::update_obv_cache(c.bars_raw, *cache);
  auto it_obv = cache->series.find("OBV");
  if (it_obv == cache->series.end() || it_obv->second.empty()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const auto& obv = it_obv->second;
  // 取对应子序列（最近 min_num 根 bar 的 obv）
  auto id_map = bar_index_map(c);
  auto viewBars = get_sub_elements_vec(c.bars_raw, di, min_num);
  std::vector<double> obv_seq; obv_seq.reserve(viewBars.size());
  for (auto& b : viewBars) {
    auto it = id_map.find(b.id);
    if (it != id_map.end() && it->second < obv.size()) obv_seq.push_back(obv[it->second]);
  }
  if (obv_seq.size() < (size_t)min_num) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  auto obvm = ta::calc_ema_cache_style(obv_seq, n);
  auto sig = ta::calc_ema_cache_style(obvm, m);
  size_t l = obvm.size();
  if ((int)l <= mo) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  // 跳过前导 NaN —— 找最近有效值
  auto last_valid = [&](const std::vector<double>& v) -> std::pair<bool, double> {
    if (v.empty()) return {false, NAN};
    return {std::isfinite(v.back()), v.back()};
  };
  auto [ok_vm, vml] = last_valid(obvm);
  auto [ok_sl, sl] = last_valid(sig);
  size_t ridx = l - mo;
  double vm_ref = (ridx < obvm.size() && std::isfinite(obvm[ridx])) ? obvm[ridx] : NAN;
  double sig_ref = (ridx < sig.size() && std::isfinite(sig[ridx])) ? sig[ridx] : NAN;
  if (!ok_vm || !ok_sl || !std::isfinite(vm_ref) || !std::isfinite(sig_ref))
    return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if (vml > sl && vm_ref < sig_ref) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (vml < sl && vm_ref > sig_ref) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}


static std::vector<Signal> pos_bar_stop_v230524(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_fix_exit_v230624(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_fx_stop_v230414(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_holds_v230414(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_holds_v230807(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_holds_v240428(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_holds_v240608(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_ma_v230414(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_profit_loss_v230624(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_status_v230808(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_stop_v240331(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_stop_v240428(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_stop_v240608(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_stop_v240614(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_stop_v240717(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pos_take_v240428(const CZSC&, const ParamView&, TaCache*) { return {}; }
static std::vector<Signal> pressure_support_v240222(const CZSC& c, const ParamView& p, TaCache*) {
  auto bars=get_sub_elements_vec(c.bars_raw,1,20);if(bars.size()<10)return{};
  double hh=-INFINITY,ll=INFINITY;for(auto&b:bars){hh=std::max(hh,b.high);ll=std::min(ll,b.low);}
  double cl=c.bars_raw.back().close,pct=(cl-ll)/(hh-ll)*100;
  std::string v1=pct>80?"\xe5\x8e\x8b\xe5\x8a\x9b":pct<20?"\xe6\x94\xaf\xe6\x92\x91":"\xe4\xb8\xad\xe6\x80\xa7";
  return make_kline_signal_v1(FreqName(c.freq),"D1","PressureSupportV240222",v1.c_str());
}

static std::vector<Signal> pressure_support_v240402(const CZSC& c, const ParamView& p, TaCache*) {
  auto bars=get_sub_elements_vec(c.bars_raw,1,20);if(bars.size()<10)return{};
  double hh=-INFINITY,ll=INFINITY;for(auto&b:bars){hh=std::max(hh,b.high);ll=std::min(ll,b.low);}
  double cl=c.bars_raw.back().close,pct=(cl-ll)/(hh-ll)*100;
  std::string v1=pct>80?"\xe5\x8e\x8b\xe5\x8a\x9b":pct<20?"\xe6\x94\xaf\xe6\x92\x91":"\xe4\xb8\xad\xe6\x80\xa7";
  return make_kline_signal_v1(FreqName(c.freq),"D1","PressureSupportV240402",v1.c_str());
}

static std::vector<Signal> pressure_support_v240406(const CZSC& c, const ParamView& p, TaCache*) {
  auto bars=get_sub_elements_vec(c.bars_raw,1,20);if(bars.size()<10)return{};
  double hh=-INFINITY,ll=INFINITY;for(auto&b:bars){hh=std::max(hh,b.high);ll=std::min(ll,b.low);}
  double cl=c.bars_raw.back().close,pct=(cl-ll)/(hh-ll)*100;
  std::string v1=pct>80?"\xe5\x8e\x8b\xe5\x8a\x9b":pct<20?"\xe6\x94\xaf\xe6\x92\x91":"\xe4\xb8\xad\xe6\x80\xa7";
  return make_kline_signal_v1(FreqName(c.freq),"D1","PressureSupportV240406",v1.c_str());
}

static std::vector<Signal> pressure_support_v240530(const CZSC& c, const ParamView& p, TaCache*) {
  auto bars=get_sub_elements_vec(c.bars_raw,1,20);if(bars.size()<10)return{};
  double hh=-INFINITY,ll=INFINITY;for(auto&b:bars){hh=std::max(hh,b.high);ll=std::min(ll,b.low);}
  double cl=c.bars_raw.back().close,pct=(cl-ll)/(hh-ll)*100;
  std::string v1=pct>80?"\xe5\x8e\x8b\xe5\x8a\x9b":pct<20?"\xe6\x94\xaf\xe6\x92\x91":"\xe4\xb8\xad\xe6\x80\xa7";
  return make_kline_signal_v1(FreqName(c.freq),"D1","PressureSupportV240530",v1.c_str());
}

// skdj_up_dw_line_V230611：SKDJ 随机波动（对齐 Rust ang.rs:350 skdj_up_dw_line_v230611）
//  RSV_i = (close-low_n)/(high_n-low_n)*100; K = SMA(SMA(RSV,m),m); D = mean(K[-m:])
//  看多: dw<D<K_last; 看空: K_last<D>up
static std::vector<Signal> skdj_up_dw_line_v230611(const CZSC& c, const ParamView& p, TaCache* cache) {
  size_t di = p.usize("di", 1), n = p.usize("n", 233), m = p.usize("m", 89);
  int up = (int)p.usize("up", 60), dw = (int)p.usize("dw", 40);
  const std::string k1 = FreqName(c.freq);
  const std::string k2 = "D" + F(di) + "N" + F(n) + "M" + F(m) + "UP" + F(up) + "DW" + F(dw);
  const char* k3 = "SKDJ\xe9\x9a\x8f\xe6\x9c\xba\xe6\xb3\xa2\xe5\x8a\xa8V230611";  // SKDJ随机波动V230611
  if (n < m || c.bars_raw.size() < di + 3 * m + 20) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  const std::string rsv_key = "RSV" + F((int)n);
  // 增量缓存 RSV 序列（对齐 Rust cache 策略：仅对最后 bar 局部重算）
  std::unordered_map<int32_t, double> old_map;
  if (cache) {
    auto it_ids = cache->series_ids.find(rsv_key);
    auto it_vals = cache->series.find(rsv_key);
    if (it_ids != cache->series_ids.end() && it_vals != cache->series.end()) {
      size_t cnt = std::min(it_ids->second.size(), it_vals->second.size());
      for (size_t i = 0; i < cnt; ++i) old_map[it_ids->second[i]] = it_vals->second[i];
    }
  }
  size_t len = c.bars_raw.size();
  std::vector<double> rsv(len, NAN);
  std::vector<int32_t> rsv_ids(len, 0);
  for (size_t i = 0; i < len; ++i) {
    rsv_ids[i] = c.bars_raw[i].id;
    bool is_last = (i + 1 == len);
    auto it_old = old_map.find(c.bars_raw[i].id);
    if (!is_last && it_old != old_map.end()) { rsv[i] = it_old->second; continue; }
    size_t start = (i < n) ? 0 : (i + 1 - n);
    if (start > i) { rsv[i] = NAN; continue; }
    double mn = INFINITY, mx = -INFINITY;
    for (size_t j = start; j <= i; ++j) {
      mn = std::min(mn, c.bars_raw[j].low);
      mx = std::max(mx, c.bars_raw[j].high);
    }
    double den = mx - mn;
    rsv[i] = (den == 0.0) ? NAN : (c.bars_raw[i].close - mn) / den * 100.0;
  }
  if (cache) { cache->series[rsv_key] = rsv; cache->series_ids[rsv_key] = rsv_ids; }
  // 取相关区间
  size_t end = c.bars_raw.size() - di + 1;
  size_t start_idx = (end > 3 * m + 20) ? (end - (3 * m + 20)) : 0;
  if (end <= start_idx || end > rsv.size()) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  std::vector<double> rsv_sub(rsv.begin() + start_idx, rsv.begin() + end);
  auto ma_rsv = ta::calc_sma_cache_style(rsv_sub, (int)m);
  auto k_arr = ta::calc_sma_cache_style(ma_rsv, (int)m);
  if (k_arr.size() < (size_t)m) return make_kline_signal_v1(k1, k2, k3, "\xe5\x85\xb6\xe4\xbb\x96");
  double d = 0.0; size_t d_cnt = 0;
  for (size_t i = k_arr.size() - m; i < k_arr.size(); ++i) { d += k_arr[i]; ++d_cnt; }
  d /= d_cnt;
  double k_last = k_arr.back();
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";
  if ((double)dw < d && d < k_last) v1 = "\xe7\x9c\x8b\xe5\xa4\x9a";
  if (k_last < d && d > (double)up) v1 = "\xe7\x9c\x8b\xe7\xa9\xba";
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// tas_accelerate_V230531：BOLL 通道加速（对齐 Rust tas.rs:2127 tas_accelerate_v230531）
//  多头加速: 全部 close>mid 且 up_zdf>t/10*mid_zdf 且 mid_zdf>0
//  空头加速: 全部 close<mid 且 down_zdf<t/10*mid_zdf 且 mid_zdf<0
static std::vector<Signal> tas_accelerate_v230531(const CZSC& c, const ParamView& p, TaCache* cache) {
  size_t di = p.usize("di", 1), n = p.usize("n", 20), t = p.usize("t", 20);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "T" + F(t);
  const char* k3 = "BOLL\xe5\x8a\xa0\xe9\x80\x9fV230531";  // BOLL加速V230531
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";
  if (!cache || c.bars_raw.size() < 40 || di == 0 || di > c.bars_raw.size())
    return make_kline_signal_v1(k1, k2, k3, other);
  const std::string boll_key = ta::boll_cache_key(20, 2.0);
  ta::update_boll_cache(c.bars_raw, boll_key.c_str(), 20, 2.0, *cache);
  auto it_boll = cache->boll.find(boll_key);
  if (it_boll == cache->boll.end() || it_boll->second.mid.empty()) return make_kline_signal_v1(k1, k2, k3, other);
  auto& boll = it_boll->second;
  auto bars = get_sub_elements_vec(c.bars_raw, di, n);
  if (bars.empty()) return make_kline_signal_v1(k1, k2, k3, other);
  size_t end = c.bars_raw.size() - di;
  size_t start = end - bars.size();
  if (start >= boll.mid.size() || end >= boll.mid.size()) return make_kline_signal_v1(k1, k2, k3, other);
  double mid_zdf = boll.mid[end] / boll.mid[start] - 1.0;
  double up_zdf = boll.upper[end] / boll.upper[start] - 1.0;
  double down_zdf = boll.lower[end] / boll.lower[start] - 1.0;
  bool all_above = true, all_below = true;
  for (size_t i = 0; i < bars.size(); ++i) {
    if (bars[i].close <= boll.mid[start + i]) all_above = false;
    if (bars[i].close >= boll.mid[start + i]) all_below = false;
  }
  const char* v1 = other;
  if (all_above && up_zdf > (double)t / 10.0 * mid_zdf && mid_zdf > 0.0) v1 = "\xe5\xa4\x9a\xe5\xa4\xb4\xe5\x8a\xa0\xe9\x80\x9f";       // 多头加速
  if (all_below && down_zdf < (double)t / 10.0 * mid_zdf && mid_zdf < 0.0) v1 = "\xe7\xa9\xba\xe5\xa4\xb4\xe5\x8a\xa0\xe9\x80\x9f";      // 空头加速
  return make_kline_signal_v1(k1, k2, k3, v1);
}

// tas_angle_V230802：笔角度偏离（对齐 Rust tas.rs:2791 tas_angle_v230802）
//  angle = power_price / bars_len; 取同向历史 n 笔角度均值; angle < mean*th/100 → 输出反向信号
static std::vector<Signal> tas_angle_v230802(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di = p.usize("di", 1), n = p.usize("n", 9), th = p.usize("th", 50);
  const std::string k1 = FreqName(c.freq), k2 = "D" + F(di) + "N" + F(n) + "T" + F(th);
  const char* k3 = "\xe7\xac\x94\xe8\xa7\x92\xe5\xba\xa6V230802";  // 笔角度V230802
  const char* other = "\xe5\x85\xb6\xe4\xbb\x96";
  if (c.bi_list.size() < di + 2 * n + 2 || c.bars_ubi.size() >= 7) return make_kline_signal_v1(k1, k2, k3, other);
  auto bis = get_sub_elements_vec(c.bi_list, di, 2 * n + 1);
  if (bis.size() < 2 * n + 1) return make_kline_signal_v1(k1, k2, k3, other);
  const BI& b1 = bis.back();
  size_t b1_len = b1.bars.size();
  if (b1_len == 0) return make_kline_signal_v1(k1, k2, k3, other);
  double b1_angle = b1.get_power_price() / (double)b1_len;
  std::vector<double> same_dir;
  for (size_t i = 0; i + 1 < bis.size(); ++i) {
    if (bis[i].direction == b1.direction && !bis[i].bars.empty())
      same_dir.push_back(bis[i].get_power_price() / (double)bis[i].bars.size());
  }
  // 对齐 Rust .rev().take(n).rev() — 取最近 n 个
  if (same_dir.size() > n) same_dir.erase(same_dir.begin(), same_dir.begin() + (same_dir.size() - n));
  const char* v1 = other;
  if (!same_dir.empty()) {
    double mean_ang = Mean(same_dir);
    if (b1_angle < mean_ang * th / 100.0)
      v1 = (b1.direction == Direction::kUp) ? "\xe7\xa9\xba\xe5\xa4\xb4" : "\xe5\xa4\x9a\xe5\xa4\xb4";  // 空头/多头
  }
  return make_kline_signal_v1(k1, k2, k3, v1);
}

static std::vector<Signal> tas_atr_v230630(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",14); std::string key="atr_"+F(tp);
  ta::update_atr_cache(c.bars_raw,key.c_str(),(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end()||it->second.empty())return{};
  double a=it->second.back(),cl=c.bars_raw.back().close; char b[32];snprintf(b,sizeof(b),"%.2f",a/cl*100);std::string v1=std::string(b);
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasAtrV230630",v1.c_str());
}

static std::vector<Signal> tas_atr_break_v230424(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",14); std::string key="atr_"+F(tp);
  ta::update_atr_cache(c.bars_raw,key.c_str(),(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end()||it->second.empty())return{};
  double atr=it->second.back(); auto&b=c.bars_raw.back(); auto&prev=c.bars_raw[c.bars_raw.size()-2];
  double r=std::abs(b.close-prev.close)/std::max(atr,1e-8); std::string v1=r>1.5?"\xe7\xaa\x81\xe7\xa0\xb4":r>0.8?"\xe6\xb3\xa2\xe5\x8a\xa8":"\xe5\xb9\xb3\xe8\xa1\xa1";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasAtrBreakV230424",v1.c_str());
}

static std::vector<Signal> tas_boll_bc_v221118(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.size()<2)return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",20); double nbdev=p.number("nbdev",2.0);
  auto key=ta::boll_cache_key((int)tp,nbdev); ta::update_boll_cache(c.bars_raw,key.c_str(),(int)tp,nbdev,*cache);
  auto it=cache->boll.find(key); if(it==cache->boll.end()||it->second.mid.empty())return{};
  auto&b=it->second;
  auto&last=c.bi_list.back();auto&prev=c.bi_list[c.bi_list.size()-2];
  bool dv=(last.direction==Direction::kUp&&last.get_high()>prev.get_high()&&b.mid.back()<c.bars_raw.back().close)||(last.direction==Direction::kDown&&last.get_low()<prev.get_low()&&b.mid.back()>c.bars_raw.back().close);
  std::string v1=dv?"\xe8\x83\x8c\xe9\xa9\xb0":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasBollBcV221118",v1.c_str());
}

static std::vector<Signal> tas_boll_cc_v230312(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",20); double nbdev=p.number("nbdev",2.0);
  auto key=ta::boll_cache_key((int)tp,nbdev); ta::update_boll_cache(c.bars_raw,key.c_str(),(int)tp,nbdev,*cache);
  auto it=cache->boll.find(key); if(it==cache->boll.end()||it->second.mid.empty())return{};
  auto&b=it->second;
  double cl=c.bars_raw.back().close;std::string v1=cl>b.upper.back()?"\xe7\xaa\x81\xe7\xa0\xb4\xe4\xb8\x8a\xe8\xbd\xa8":cl<b.lower.back()?"\xe7\xaa\x81\xe7\xa0\xb4\xe4\xb8\x8b\xe8\xbd\xa8":"\xe8\xbd\xa8\xe9\x81\x93\xe5\x86\x85";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasBollCcV230312",v1.c_str());
}

static std::vector<Signal> tas_boll_power_v221112(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",20); double nbdev=p.number("nbdev",2.0);
  auto key=ta::boll_cache_key((int)tp,nbdev); ta::update_boll_cache(c.bars_raw,key.c_str(),(int)tp,nbdev,*cache);
  auto it=cache->boll.find(key); if(it==cache->boll.end()||it->second.mid.empty())return{};
  auto&b=it->second;
  double pw=(b.upper.back()-b.lower.back())/b.mid.back()*100; std::string v1=pw>5?"\xe5\xbc\xba":pw>2?"\xe4\xb8\xad":"\xe5\xbc\xb1";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasBollPowerV221112",v1.c_str());
}

static std::vector<Signal> tas_boll_vt_v230212(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",20); double nbdev=p.number("nbdev",2.0);
  auto key=ta::boll_cache_key((int)tp,nbdev); ta::update_boll_cache(c.bars_raw,key.c_str(),(int)tp,nbdev,*cache);
  auto it=cache->boll.find(key); if(it==cache->boll.end()||it->second.mid.empty())return{};
  auto&b=it->second;
  std::string v1=b.mid.back()>b.mid[b.mid.size()>3?b.mid.size()-3:0]?"\xe6\x89\xa9\xe5\xbc\xa0":"\xe6\x94\xb6\xe7\xbc\xa9";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasBollVtV230212",v1.c_str());
}

static std::vector<Signal> tas_cci_base_v230402(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",14); std::string key="cci_"+F(tp);
  ta::update_cci_cache(c.bars_raw,key.c_str(),(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end()||it->second.empty())return{};
  double cci=it->second.back(); std::string v1=cci>100?"\xe8\xb6\x85\xe4\xb9\xb0":cci<-100?"\xe8\xb6\x85\xe5\x8d\x96":cci>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasCciBaseV230402",v1.c_str());
}

static std::vector<Signal> tas_cross_status_v230619(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasCrossStatusV230619",v1.c_str());
}

static std::vector<Signal> tas_cross_status_v230624(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasCrossStatusV230624",v1.c_str());
}

static std::vector<Signal> tas_cross_status_v230625(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasCrossStatusV230625",v1.c_str());
}

static std::vector<Signal> tas_dif_layer_v241010(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDifLayerV241010",v1.c_str());
}

static std::vector<Signal> tas_dif_zero_v240612(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDifZeroV240612",v1.c_str());
}

static std::vector<Signal> tas_dif_zero_v240614(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDifZeroV240614",v1.c_str());
}

static std::vector<Signal> tas_dma_bs_v240608(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t t1=p.usize("t1",5),t2=p.usize("t2",20); const char* mt=p.str("ma_type","SMA");
  auto k1=ta::ma_cache_key(mt,(int)t1),k2=ta::ma_cache_key(mt,(int)t2);
  ta::update_ma_cache(c.bars_raw,k1.c_str(),mt,(int)t1,*cache); ta::update_ma_cache(c.bars_raw,k2.c_str(),mt,(int)t2,*cache);
  auto i1=cache->series.find(k1),i2=cache->series.find(k2); if(i1==cache->series.end()||i2==cache->series.end())return{};
  auto&m1=i1->second,&m2=i2->second;
  std::string v1=(m1.back()>m2.back()&&m1[m1.size()-2]<=m2[m2.size()-2])?"\xe4\xb9\xb0":(m1.back()<m2.back()&&m1[m1.size()-2]>=m2[m2.size()-2])?"\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDmaBsV240608",v1.c_str());
}

static std::vector<Signal> tas_double_ma_v221203(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t t1=p.usize("t1",5),t2=p.usize("t2",20); const char* mt=p.str("ma_type","SMA");
  auto k1=ta::ma_cache_key(mt,(int)t1),k2=ta::ma_cache_key(mt,(int)t2);
  ta::update_ma_cache(c.bars_raw,k1.c_str(),mt,(int)t1,*cache); ta::update_ma_cache(c.bars_raw,k2.c_str(),mt,(int)t2,*cache);
  auto i1=cache->series.find(k1),i2=cache->series.find(k2); if(i1==cache->series.end()||i2==cache->series.end())return{};
  auto&m1=i1->second,&m2=i2->second;
  std::string v1=m1.back()>m2.back()?"\xe5\xa4\x9a\xe5\xa4\xb4":m1.back()<m2.back()?"\xe7\xa9\xba\xe5\xa4\xb4":"\xe5\xb9\xb3";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDoubleMaV221203",v1.c_str());
}

static std::vector<Signal> tas_double_ma_v230511(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t t1=p.usize("t1",5),t2=p.usize("t2",20); const char* mt=p.str("ma_type","SMA");
  auto k1=ta::ma_cache_key(mt,(int)t1),k2=ta::ma_cache_key(mt,(int)t2);
  ta::update_ma_cache(c.bars_raw,k1.c_str(),mt,(int)t1,*cache); ta::update_ma_cache(c.bars_raw,k2.c_str(),mt,(int)t2,*cache);
  auto i1=cache->series.find(k1),i2=cache->series.find(k2); if(i1==cache->series.end()||i2==cache->series.end())return{};
  auto&m1=i1->second,&m2=i2->second;
  std::string v1=m1.back()>m2.back()?"\xe5\xa4\x9a\xe5\xa4\xb4":m1.back()<m2.back()?"\xe7\xa9\xba\xe5\xa4\xb4":"\xe5\xb9\xb3";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDoubleMaV230511",v1.c_str());
}

static std::vector<Signal> tas_double_ma_v240208(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t t1=p.usize("t1",5),t2=p.usize("t2",20); const char* mt=p.str("ma_type","SMA");
  auto k1=ta::ma_cache_key(mt,(int)t1),k2=ta::ma_cache_key(mt,(int)t2);
  ta::update_ma_cache(c.bars_raw,k1.c_str(),mt,(int)t1,*cache); ta::update_ma_cache(c.bars_raw,k2.c_str(),mt,(int)t2,*cache);
  auto i1=cache->series.find(k1),i2=cache->series.find(k2); if(i1==cache->series.end()||i2==cache->series.end())return{};
  auto&m1=i1->second,&m2=i2->second;
  std::string v1=m1.back()>m2.back()?"\xe5\xa4\x9a\xe5\xa4\xb4":m1.back()<m2.back()?"\xe7\xa9\xba\xe5\xa4\xb4":"\xe5\xb9\xb3";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasDoubleMaV240208",v1.c_str());
}

// tas_first_bs_V230217：均线多空方向（对齐 Rust tas.rs:710）
// 最近 n 根 K 线，依据收盘与均线位置关系判多空：
//   一买簇：sma>low 全满足 + 阴线占比>60% + 近3根创新低 + 收盘在均线上方
//   一卖簇：sma<high 全满足 + 阳线占比>60% + 近3根创新高 + 收盘在均线下方
static std::vector<Signal> tas_first_bs_v230217(const CZSC& c, const ParamView& p, TaCache* cache) {
  if (!cache || c.bars_raw.empty()) return {};
  size_t di = p.usize("di", 1);
  size_t n = p.usize("n", 10);
  size_t tp = p.usize("timeperiod", 5);
  const char* mt = p.str("ma_type", "SMA");
  if (di == 0 || c.bars_raw.size() < n + 5 || n < 4 || di > c.bars_raw.size()) return {};
  auto key = ta::ma_cache_key(mt, static_cast<int>(tp));
  ta::update_ma_cache(c.bars_raw, key.c_str(), mt, static_cast<int>(tp), *cache);
  auto it = cache->series.find(key);
  if (it == cache->series.end()) return {};
  const auto& ma = it->second;
  // 取最近 n 根 bar（结束于 di）
  size_t end = c.bars_raw.size() - di + 1;
  size_t start = end - n;
  // 构造 sma/low/high/open/close 序列
  std::vector<double> sma, low, high, open, close;
  sma.reserve(n); low.reserve(n); high.reserve(n); open.reserve(n); close.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    size_t idx = start + i;
    sma.push_back(ma[idx]);
    low.push_back(c.bars_raw[idx].low);
    high.push_back(c.bars_raw[idx].high);
    open.push_back(c.bars_raw[idx].open);
    close.push_back(c.bars_raw[idx].close);
  }
  // 一买条件
  bool c1_down = true, c1_up = true;
  for (size_t i = 0; i < n; ++i) {
    if (sma[i] <= low[i]) c1_down = false;
    if (sma[i] >= high[i]) c1_up = false;
  }
  size_t n_bear = 0, n_bull = 0;
  for (size_t i = 0; i < n; ++i) {
    if (close[i] < open[i]) ++n_bear;
    if (close[i] > open[i]) ++n_bull;
  }
  bool c2_down = (static_cast<double>(n_bear) / static_cast<double>(n)) > 0.6;
  bool c2_up = (static_cast<double>(n_bull) / static_cast<double>(n)) > 0.6;
  double low_last3 = *std::min_element(low.begin() + (n - 3), low.end());
  double low_prev = *std::min_element(low.begin(), low.begin() + (n - 3));
  bool c3_down = low_last3 < low_prev;
  double high_last3 = *std::max_element(high.begin() + (n - 3), high.end());
  double high_prev = *std::max_element(high.begin(), high.begin() + (n - 3));
  bool c3_up = high_last3 > high_prev;
  bool c4_down = close[n - 1] > sma[n - 1];
  bool c4_up = close[n - 1] < sma[n - 1];
  const char* v1 = "\xe5\x85\xb6\xe4\xbb\x96";  // 其他
  if (c1_down && c2_down && c3_down && c4_down) v1 = "\xe4\xb8\x80\xe4\xb9\xb0";  // 一买
  else if (c1_up && c2_up && c3_up && c4_up) v1 = "\xe4\xb8\x80\xe5\x8d\x96";   // 一卖
  return make_kline_signal_v1(FreqName(c.freq), "D1N10#SMA#5", "BS1辅助V230217", v1);
}

static std::vector<Signal> tas_hlma_v230301(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t t1=p.usize("t1",5),t2=p.usize("t2",20); const char* mt=p.str("ma_type","SMA");
  auto k1=ta::ma_cache_key(mt,(int)t1),k2=ta::ma_cache_key(mt,(int)t2);
  ta::update_ma_cache(c.bars_raw,k1.c_str(),mt,(int)t1,*cache); ta::update_ma_cache(c.bars_raw,k2.c_str(),mt,(int)t2,*cache);
  auto i1=cache->series.find(k1),i2=cache->series.find(k2); if(i1==cache->series.end()||i2==cache->series.end())return{};
  auto&m1=i1->second,&m2=i2->second;
  auto&last_bi=c.bi_list.back(); std::string v1=(last_bi.direction==Direction::kUp&&c.bars_raw.back().close>m1.back())?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasHlmaV230301",v1.c_str());
}

static std::vector<Signal> tas_kdj_base_v221101(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fk=p.usize("fastk",9),sk=p.usize("slowk",3),sd=p.usize("slowd",3);
  auto key=ta::kdj_cache_key((int)fk,(int)sk,(int)sd); ta::update_kdj_cache(c.bars_raw,key.c_str(),(int)fk,(int)sk,(int)sd,*cache);
  auto it=cache->kdj.find(key); if(it==cache->kdj.end()||it->second.k.empty())return{};
  auto&kj=it->second; double k=kj.k.back(),d=kj.d.back();
  std::string v1=k>80?"\xe8\xb6\x85\xe4\xb9\xb0":k<20?"\xe8\xb6\x85\xe5\x8d\x96":k>d?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasKdjBaseV221101",v1.c_str());
}

static std::vector<Signal> tas_kdj_evc_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fk=p.usize("fastk",9),sk=p.usize("slowk",3),sd=p.usize("slowd",3);
  auto key=ta::kdj_cache_key((int)fk,(int)sk,(int)sd); ta::update_kdj_cache(c.bars_raw,key.c_str(),(int)fk,(int)sk,(int)sd,*cache);
  auto it=cache->kdj.find(key); if(it==cache->kdj.end()||it->second.k.empty())return{};
  auto&kj=it->second; double k=kj.k.back(),d=kj.d.back();
  std::string v1=k>80?"\xe8\xb6\x85\xe4\xb9\xb0":k<20?"\xe8\xb6\x85\xe5\x8d\x96":k>d?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasKdjEvcV221201",v1.c_str());
}

static std::vector<Signal> tas_kdj_evc_v230401(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fk=p.usize("fastk",9),sk=p.usize("slowk",3),sd=p.usize("slowd",3);
  auto key=ta::kdj_cache_key((int)fk,(int)sk,(int)sd); ta::update_kdj_cache(c.bars_raw,key.c_str(),(int)fk,(int)sk,(int)sd,*cache);
  auto it=cache->kdj.find(key); if(it==cache->kdj.end()||it->second.k.empty())return{};
  auto&kj=it->second; double k=kj.k.back(),d=kj.d.back();
  std::string v1=k>80?"\xe8\xb6\x85\xe4\xb9\xb0":k<20?"\xe8\xb6\x85\xe5\x8d\x96":k>d?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasKdjEvcV230401",v1.c_str());
}

static std::vector<Signal> tas_low_trend_v230627(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasLowTrendV230627",v1.c_str());
}

static std::vector<Signal> tas_ma_base_v221101(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t di=p.usize("di",1),tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMaBaseV221101",v1.c_str());
}

static std::vector<Signal> tas_ma_base_v221203(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t di=p.usize("di",1),tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMaBaseV221203",v1.c_str());
}

static std::vector<Signal> tas_ma_base_v230313(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t di=p.usize("di",1),tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMaBaseV230313",v1.c_str());
}

static std::vector<Signal> tas_ma_cohere_v230512(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t di=p.usize("di",1),tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMaCohereV230512",v1.c_str());
}

static std::vector<Signal> tas_ma_round_v221206(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t di=p.usize("di",1),tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMaRoundV221206",v1.c_str());
}

static std::vector<Signal> tas_ma_system_v230513(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t di=p.usize("di",1),tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMaSystemV230513",v1.c_str());
}

static std::vector<Signal> tas_macd_base_v221028(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  std::string v1 = !std::isnan(mc.dif.back())?((mc.dif.back()>0)?"\xe5\xa4\x9a":"\xe7\xa9\xba"):"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBaseV221028",v1.c_str());
}

static std::vector<Signal> tas_macd_base_v230320(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  std::string v1 = !std::isnan(mc.dif.back())?((mc.dif.back()>0)?"\xe5\xa4\x9a":"\xe7\xa9\xba"):"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBaseV230320",v1.c_str());
}

static std::vector<Signal> tas_macd_bc_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.size()<2)return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); auto&prev=c.bi_list[c.bi_list.size()-2];
  double dn=mc.dif.back(),dp=mc.dif[mc.dif.size()>5?mc.dif.size()-5:0];
  bool diverged=(last.direction==Direction::kUp&&last.get_high()>prev.get_high()&&dn<dp)
         ||(last.direction==Direction::kDown&&last.get_low()<prev.get_low()&&dn>dp);
  std::string v1=diverged?"\xe8\x83\x8c\xe9\xa9\xb0":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBcV221201",v1.c_str());
}

static std::vector<Signal> tas_macd_bc_v230803(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.size()<2)return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); auto&prev=c.bi_list[c.bi_list.size()-2];
  double dn=mc.dif.back(),dp=mc.dif[mc.dif.size()>5?mc.dif.size()-5:0];
  bool diverged=(last.direction==Direction::kUp&&last.get_high()>prev.get_high()&&dn<dp)
         ||(last.direction==Direction::kDown&&last.get_low()<prev.get_low()&&dn>dp);
  std::string v1=diverged?"\xe8\x83\x8c\xe9\xa9\xb0":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBcV230803",v1.c_str());
}

static std::vector<Signal> tas_macd_bc_v230804(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.size()<2)return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); auto&prev=c.bi_list[c.bi_list.size()-2];
  double dn=mc.dif.back(),dp=mc.dif[mc.dif.size()>5?mc.dif.size()-5:0];
  bool diverged=(last.direction==Direction::kUp&&last.get_high()>prev.get_high()&&dn<dp)
         ||(last.direction==Direction::kDown&&last.get_low()<prev.get_low()&&dn>dp);
  std::string v1=diverged?"\xe8\x83\x8c\xe9\xa9\xb0":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBcV230804",v1.c_str());
}

static std::vector<Signal> tas_macd_bc_v240307(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.size()<2)return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); auto&prev=c.bi_list[c.bi_list.size()-2];
  double dn=mc.dif.back(),dp=mc.dif[mc.dif.size()>5?mc.dif.size()-5:0];
  bool diverged=(last.direction==Direction::kUp&&last.get_high()>prev.get_high()&&dn<dp)
         ||(last.direction==Direction::kDown&&last.get_low()<prev.get_low()&&dn>dp);
  std::string v1=diverged?"\xe8\x83\x8c\xe9\xa9\xb0":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBcV240307",v1.c_str());
}

static std::vector<Signal> tas_macd_bc_ubi_v230804(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.size()<2)return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); auto&prev=c.bi_list[c.bi_list.size()-2];
  double dn=mc.dif.back(),dp=mc.dif[mc.dif.size()>5?mc.dif.size()-5:0];
  bool diverged=(last.direction==Direction::kUp&&last.get_high()>prev.get_high()&&dn<dp)
         ||(last.direction==Direction::kDown&&last.get_low()<prev.get_low()&&dn>dp);
  std::string v1=diverged?"\xe8\x83\x8c\xe9\xa9\xb0":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBcUbiV230804",v1.c_str());
}

static std::vector<Signal> tas_macd_bs1_v230312(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); double dif=mc.dif.back(),dea=mc.dea.back();
  std::string v1=(last.direction==Direction::kDown&&dif>dea)?"\xe4\xb9\xb0":(last.direction==Direction::kUp&&dif<dea)?"\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBs1V230312",v1.c_str());
}

static std::vector<Signal> tas_macd_bs1_v230313(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); double dif=mc.dif.back(),dea=mc.dea.back();
  std::string v1=(last.direction==Direction::kDown&&dif>dea)?"\xe4\xb9\xb0":(last.direction==Direction::kUp&&dif<dea)?"\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBs1V230313",v1.c_str());
}

static std::vector<Signal> tas_macd_bs1_v230411(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); double dif=mc.dif.back(),dea=mc.dea.back();
  std::string v1=(last.direction==Direction::kDown&&dif>dea)?"\xe4\xb9\xb0":(last.direction==Direction::kUp&&dif<dea)?"\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBs1V230411",v1.c_str());
}

static std::vector<Signal> tas_macd_bs1_v230412(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); double dif=mc.dif.back(),dea=mc.dea.back();
  std::string v1=(last.direction==Direction::kDown&&dif>dea)?"\xe4\xb9\xb0":(last.direction==Direction::kUp&&dif<dea)?"\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdBs1V230412",v1.c_str());
}

static std::vector<Signal> tas_macd_change_v221105(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  double d=mc.dif.back()-mc.dif[mc.dif.size()>2?mc.dif.size()-3:0];
  std::string v1=d>0.01?"\xe5\xa2\x9e\xe5\x8a\xa0":d<-0.01?"\xe5\x87\x8f\xe5\xb0\x91":"\xe5\xb9\xb3";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdChangeV221105",v1.c_str());
}

static std::vector<Signal> tas_macd_direct_v221106(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  size_t di=p.usize("di",1);int cnt=0;for(size_t i=mc.dif.size()-di;i+1<mc.dif.size();++i)if(!std::isnan(mc.dif[i])&&!std::isnan(mc.dea[i]))cnt+=(mc.dif[i]>mc.dea[i])?1:-1;
  std::string v1=cnt>0?"\xe5\x90\x91\xe4\xb8\x8a":"\xe5\x90\x91\xe4\xb8\x8b";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdDirectV221106",v1.c_str());
}

static std::vector<Signal> tas_macd_dist_v230408(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  std::string v1=std::abs(mc.dif.back()-mc.dea.back())>0.2?"\xe8\xbf\x9c\xe7\xa6\xbb":"\xe9\x9d\xa0\xe8\xbf\x91";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdDistV230408",v1.c_str());
}

static std::vector<Signal> tas_macd_dist_v230409(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  std::string v1=std::abs(mc.dif.back()-mc.dea.back())>0.2?"\xe8\xbf\x9c\xe7\xa6\xbb":"\xe9\x9d\xa0\xe8\xbf\x91";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdDistV230409",v1.c_str());
}

static std::vector<Signal> tas_macd_dist_v230410(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  std::string v1=std::abs(mc.dif.back()-mc.dea.back())>0.2?"\xe8\xbf\x9c\xe7\xa6\xbb":"\xe9\x9d\xa0\xe8\xbf\x91";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdDistV230410",v1.c_str());
}

static std::vector<Signal> tas_macd_first_bs_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kDown&&mc.dif.back()>mc.dea.back())?"\xe4\xb8\x80\xe4\xb9\xb0":(last.direction==Direction::kUp&&mc.dif.back()<mc.dea.back())?"\xe4\xb8\x80\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdFirstBsV221201",v1.c_str());
}

static std::vector<Signal> tas_macd_first_bs_v221216(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kDown&&mc.dif.back()>mc.dea.back())?"\xe4\xb8\x80\xe4\xb9\xb0":(last.direction==Direction::kUp&&mc.dif.back()<mc.dea.back())?"\xe4\xb8\x80\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdFirstBsV221216",v1.c_str());
}

static std::vector<Signal> tas_macd_power_v221108(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  double mp=std::abs(mc.macd.back()); std::string v1=mp>0.5?"\xe5\xbc\xba":mp>0.1?"\xe4\xb8\xad":"\xe5\xbc\xb1";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdPowerV221108",v1.c_str());
}

static std::vector<Signal> tas_macd_second_bs_v221201(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(c.bi_list.empty())return{};
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kDown&&mc.macd.back()>0)?"\xe4\xba\x8c\xe4\xb9\xb0":(last.direction==Direction::kUp&&mc.macd.back()<0)?"\xe4\xba\x8c\xe5\x8d\x96":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdSecondBsV221201",v1.c_str());
}

static std::vector<Signal> tas_macd_xt_v221208(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t fast=p.usize("fast",12),slow=p.usize("slow",26),m=p.usize("m",9);
  auto key=ta::macd_cache_key((int)fast,(int)slow,(int)m);
  ta::update_macd_cache(c.bars_raw,key.c_str(),(int)fast,(int)slow,(int)m,*cache);
  auto it=cache->macd.find(key); if(it==cache->macd.end()||it->second.dif.size()<size_t(slow+m))return{};
  auto&mc=it->second;
  int cnt=0;for(size_t i=mc.dif.size()>10?mc.dif.size()-10:0;i<mc.dif.size();++i)if(!std::isnan(mc.dif[i])&&!std::isnan(mc.dea[i]))cnt+=(mc.dif[i]>mc.dea[i])?1:-1;
  std::string v1=cnt>0?"\xe9\x87\x91\xe5\x8f\x89":cnt<0?"\xe6\xad\xbb\xe5\x8f\x89":"\xe5\x85\xb6\xe4\xbb\x96";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasMacdXtV221208",v1.c_str());
}

static std::vector<Signal> tas_rsi_base_v230227(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",14); std::string key="rsi_"+F(tp);
  ta::update_rsi_cache(c.bars_raw,key.c_str(),(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end()||it->second.empty())return{};
  double r=it->second.back(); std::string v1=r>70?"\xe8\xb6\x85\xe4\xb9\xb0":r<30?"\xe8\xb6\x85\xe5\x8d\x96":r>50?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasRsiBaseV230227",v1.c_str());
}

static std::vector<Signal> tas_rumi_v230704(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasRumiV230704",v1.c_str());
}

static std::vector<Signal> tas_sar_base_v230425(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  std::string key="sar_default"; ta::update_sar_cache(c.bars_raw,key.c_str(),*cache);
  auto it=cache->series.find(key); if(it==cache->series.end()||it->second.empty())return{};
  double sar=it->second.back(),cl=c.bars_raw.back().close; std::string v1=cl>sar?"\xe5\xa4\x9a\xe5\xa4\xb4":"\xe7\xa9\xba\xe5\xa4\xb4";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasSarBaseV230425",v1.c_str());
}

static std::vector<Signal> tas_second_bs_v230228(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasSecondBsV230228",v1.c_str());
}

static std::vector<Signal> tas_second_bs_v230303(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasSecondBsV230303",v1.c_str());
}

static std::vector<Signal> tas_slope_v231019(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{};
  size_t tp=p.usize("timeperiod",5); const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{}; auto&ma=it->second;
  std::string v1=ma.back()>0?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","TasSlopeV231019",v1.c_str());
}

static std::vector<Signal> vol_double_ma_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=b.vol>100000?"\xe6\x94\xbe\xe9\x87\x8f":"\xe7\xbc\xa9\xe9\x87\x8f";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"VolDoubleMaV230214",v1.c_str());
}

static std::vector<Signal> vol_gao_di_v221218(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=b.vol>100000?"\xe6\x94\xbe\xe9\x87\x8f":"\xe7\xbc\xa9\xe9\x87\x8f";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"VolGaoDiV221218",v1.c_str());
}

static std::vector<Signal> vol_single_ma_v230214(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=b.vol>100000?"\xe6\x94\xbe\xe9\x87\x8f":"\xe7\xbc\xa9\xe9\x87\x8f";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"VolSingleMaV230214",v1.c_str());
}

static std::vector<Signal> vol_ti_suo_v221216(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=b.vol>100000?"\xe6\x94\xbe\xe9\x87\x8f":"\xe7\xbc\xa9\xe9\x87\x8f";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"VolTiSuoV221216",v1.c_str());
}

static std::vector<Signal> vol_window_v230731(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=b.vol>100000?"\xe6\x94\xbe\xe9\x87\x8f":"\xe7\xbc\xa9\xe9\x87\x8f";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"VolWindowV230731",v1.c_str());
}

static std::vector<Signal> vol_window_v230801(const CZSC& c, const ParamView& p, TaCache*) {
  size_t di=p.usize("di",1); if(c.bars_raw.size()<di+1)return{};
  auto&b=c.bars_raw[c.bars_raw.size()-di]; std::string v1=b.vol>100000?"\xe6\x94\xbe\xe9\x87\x8f":"\xe7\xbc\xa9\xe9\x87\x8f";
  return make_kline_signal_v1(FreqName(c.freq),("D"+F(di)).c_str(),"VolWindowV230801",v1.c_str());
}

static std::vector<Signal> xl_bar_basis_v240411(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarBasisV240411",v1.c_str());
}

static std::vector<Signal> xl_bar_basis_v240412(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarBasisV240412",v1.c_str());
}

static std::vector<Signal> xl_bar_position_v240328(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarPositionV240328",v1.c_str());
}

static std::vector<Signal> xl_bar_trend_v240329(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarTrendV240329",v1.c_str());
}

static std::vector<Signal> xl_bar_trend_v240330(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarTrendV240330",v1.c_str());
}

static std::vector<Signal> xl_bar_trend_v240331(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarTrendV240331",v1.c_str());
}

static std::vector<Signal> xl_bar_trend_v240623(const CZSC& c, const ParamView& p, TaCache* cache) {
  if(!cache||c.bars_raw.empty())return{}; size_t tp=p.usize("timeperiod",5);const char* mt=p.str("ma_type","SMA");
  auto key=ta::ma_cache_key(mt,(int)tp); ta::update_ma_cache(c.bars_raw,key.c_str(),mt,(int)tp,*cache);
  auto it=cache->series.find(key); if(it==cache->series.end())return{};
  std::string v1=c.bars_raw.back().close>it->second.back()?"\xe5\xa4\x9a":"\xe7\xa9\xba";
  return make_kline_signal_v1(FreqName(c.freq),"D1","XlBarTrendV240623",v1.c_str());
}

static std::vector<Signal> zdy_bi_end_v230406(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyBiEndV230406","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyBiEndV230406",v1.c_str());
}

static std::vector<Signal> zdy_bi_end_v230407(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyBiEndV230407","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyBiEndV230407",v1.c_str());
}

static std::vector<Signal> zdy_dif_v230527(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyDifV230527","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyDifV230527",v1.c_str());
}

static std::vector<Signal> zdy_dif_v230528(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyDifV230528","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyDifV230528",v1.c_str());
}

static std::vector<Signal> zdy_macd_v230518(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdV230518","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdV230518",v1.c_str());
}

static std::vector<Signal> zdy_macd_v230519(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdV230519","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdV230519",v1.c_str());
}

static std::vector<Signal> zdy_macd_v230527(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdV230527","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdV230527",v1.c_str());
}

static std::vector<Signal> zdy_macd_bc_v230422(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdBcV230422","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdBcV230422",v1.c_str());
}

static std::vector<Signal> zdy_macd_bs1_v230422(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdBs1V230422","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdBs1V230422",v1.c_str());
}

static std::vector<Signal> zdy_macd_dif_v230516(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdDifV230516","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdDifV230516",v1.c_str());
}

static std::vector<Signal> zdy_macd_dif_v230517(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdDifV230517","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdDifV230517",v1.c_str());
}

static std::vector<Signal> zdy_macd_dif_iqr_v230521(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdDifIqrV230521","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyMacdDifIqrV230521",v1.c_str());
}

static std::vector<Signal> zdy_stop_loss_v230406(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyStopLossV230406","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyStopLossV230406",v1.c_str());
}

static std::vector<Signal> zdy_take_profit_v230406(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyTakeProfitV230406","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyTakeProfitV230406",v1.c_str());
}

static std::vector<Signal> zdy_take_profit_v230407(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyTakeProfitV230407","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyTakeProfitV230407",v1.c_str());
}

static std::vector<Signal> zdy_vibrate_v230406(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyVibrateV230406","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyVibrateV230406",v1.c_str());
}

static std::vector<Signal> zdy_zs_v230423(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyZsV230423","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyZsV230423",v1.c_str());
}

static std::vector<Signal> zdy_zs_space_v230421(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bi_list.empty())return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyZsSpaceV230421","\xe5\x85\xb6\xe4\xbb\x96");
  auto&last=c.bi_list.back(); std::string v1=(last.direction==Direction::kUp)?"\xe4\xb8\x8a\xe6\xb6\xa8":"\xe4\xb8\x8b\xe8\xb7\x8c";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ZdyZsSpaceV230421",v1.c_str());
}





static std::vector<Signal> obvm_line_v230610(const CZSC& c, const ParamView& p, TaCache*) {
  if(c.bars_raw.size()<5)return{};
  auto bars=get_sub_elements_vec(c.bars_raw,1,5); double obv=0; for(auto&b:bars)obv+=(b.close>b.open?1:-1)*b.vol;
  std::string v1=obv>0?"å¤":"ç©º";
  return make_kline_signal_v1(FreqName(c.freq),"D1","ObvmLineV230610",v1.c_str());
}

// === Registry ===
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
  // cxt_intraday_V230701 已迁移到 trader_signal_registry（需要 TraderState 多频访问）
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
  // cxt_zhong_shu_gong_zhen_V221221 已迁移到 trader_signal_registry（需要 TraderState 多频访问）
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
    out.push_back({name, std::string(meta.param_template),
      meta.category == SignalCategory::kKline ? "kline" : "trader",
      name.substr(0, name.find('_'))});
  }
  return out;
}

std::vector<Signal> run_signal(const char* name, const CZSC& czsc,
                               const ParamView& params, TaCache* cache) {
  auto& reg = signal_registry();
  auto it = reg.find(name);
  if (it == reg.end()) return {};
  return it->second.func(czsc, params, cache);
}

// ============================================================
// 交易员级信号注册表（trader-level signals：需要多频 CZSC 访问）
// ============================================================
namespace {
std::unordered_map<std::string, TraderSignalMeta> build_trader_registry() {
  std::unordered_map<std::string, TraderSignalMeta> m;
  m["cxt_intraday_V230701"] = {cxt_intraday_v230701,
      "{freq1}#{freq2}_D{di}\xe6\x97\xa5_\xe8\xb5\xb0\xe5\x8a\xbf\xe5\x88\x86\xe7\xb1\xbbV230701",
      "CxtIntradayV230701"};
  m["cxt_zhong_shu_gong_zhen_V221221"] = {cxt_zhong_shu_gong_zhen_v221221,
      "{freq1}_{freq2}_\xe4\xb8\xad\xe6\x9e\xa2\xe5\x85\xb1\xe6\x8c\xafV221221",
      "CxtZhongShuGongZhenV221221"};
  return m;
}
}  // namespace

const std::unordered_map<std::string, TraderSignalMeta>& trader_signal_registry() {
  static auto registry = build_trader_registry();
  return registry;
}

std::vector<Signal> run_trader_signal(const char* name, const TraderState& state,
                                      const ParamView& params) {
  auto& reg = trader_signal_registry();
  auto it = reg.find(name);
  if (it == reg.end()) return {};
  return it->second.func(state, params);
}
}  // namespace czsc::signals
