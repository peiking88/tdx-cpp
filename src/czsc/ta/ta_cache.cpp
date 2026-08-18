// TaCache 增量缓存更新逻辑
// 对齐 Rust ~/peiking88/czsc/crates/czsc-signals/src/utils/ta.rs:393-1154
#include "czsc/ta/indicators.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace czsc::ta {

// 增量/全量切换窗口常量（对齐 Rust ta.rs 经验阈值）。
constexpr size_t kFullRecomputeExtra = 15;        // 数据量 < period + 此值 → 全量
constexpr size_t kIncrementalWindowExtra = 10;    // 增量窗口回看附加量
constexpr size_t kAtrWindowExtra = 80;            // ATR 增量窗口附加量
constexpr int    kMacdMinCountExtra = 168;        // MACD 最小样本附加量
constexpr size_t kSarFullThreshold = 50;          // SAR 低于此数据量 → 全量
constexpr size_t kSarWindowMax = 120;             // SAR 增量窗口上限
constexpr double kSarAccelInit = 0.02;            // SAR 起始加速因子
constexpr double kSarAccelMax = 0.2;              // SAR 最大加速因子
constexpr int    kMaTrailCount = 5;               // MA/Vol/MACD/BOLL/KDJ/RSI 回写根数
constexpr int    kMaVolTrailCount = 3;            // 成交量 MA 回写根数（量噪声更大）

// --- 通用：提取 bar ids（不依赖 CZSC 类）---
static std::vector<int32_t> extract_ids(const std::vector<RawBar>& bars) {
  std::vector<int32_t> ids(bars.size());
  for (size_t i = 0; i < bars.size(); ++i) ids[i] = bars[i].id;
  return ids;
}

// --- 通用：判断是否需要全量重算 ---
static bool need_full_init(const std::vector<int32_t>& bar_ids, size_t now_len,
                           int min_count, TaCache& cache,
                           const std::unordered_map<std::string, std::vector<int32_t>>& id_map,
                           const std::string& key) {
  auto it = id_map.find(key);
  if (it == id_map.end()) return true;
  if (now_len < 2 || it->second.empty()) return true;
  int32_t penultimate = bar_ids[now_len - 2];
  return std::find(it->second.begin(), it->second.end(), penultimate) == it->second.end();
}

// --- 通用：对齐旧缓存，新 bar 填 NaN ---
static std::vector<double> align_series(
    const std::vector<int32_t>& bar_ids,
    const std::vector<int32_t>& existing_ids,
    const std::vector<double>& existing) {
  std::unordered_map<int32_t, double> old;
  for (size_t i = 0; i < existing_ids.size(); ++i)
    old[existing_ids[i]] = existing[i];
  std::vector<double> res(bar_ids.size(), NAN);
  for (size_t i = 0; i < bar_ids.size(); ++i) {
    auto it = old.find(bar_ids[i]);
    if (it != old.end()) res[i] = it->second;
  }
  return res;
}

// ============================================================
// update_ma_cache（ta.rs:393-464）/ update_vol_ma_cache（ta.rs:467-538）
//
// 两函数 body 完全一致，仅 ① 取值字段（close vs vol）、② 填充窗口大小（kMaTrailCount vs kMaVolTrailCount）
// 不同。抽出 update_ma_like_cache 共享，两个 public signature 各保留一行包装以维持 API。
// ============================================================
template <typename FieldFn>
static void update_ma_like_cache(const std::vector<RawBar>& bars_raw,
                                 const char* cache_key,
                                 const char* ma_type, int period, TaCache& cache,
                                 FieldFn field_at, int trail_count) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);

  bool full = !cache.series.count(cache_key) || now_len < static_cast<size_t>(period + kFullRecomputeExtra);
  if (!full) full = need_full_init(bar_ids, now_len, period, cache, cache.series_ids, cache_key);

  std::vector<double> vals(now_len);
  for (size_t i = 0; i < now_len; ++i) vals[i] = field_at(bars_raw[i]);

  if (full) {
    std::vector<double> res;
    if (strcasecmp(ma_type, "EMA") == 0) res = ema(vals.data(), now_len, period);
    else if (strcasecmp(ma_type, "WMA") == 0) res = wma(vals.data(), now_len, period);
    else res = sma(vals.data(), now_len, period);
    cache.series[cache_key] = res;
    cache.series_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  auto res = align_series(bar_ids, cache.series_ids[cache_key], cache.series[cache_key]);
  size_t ws = std::min(static_cast<size_t>(period + kIncrementalWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> part(vals.begin() + ws_start, vals.end());
  std::vector<double> partial;
  if (strcasecmp(ma_type, "EMA") == 0) partial = ema(part.data(), ws, period);
  else if (strcasecmp(ma_type, "WMA") == 0) partial = wma(part.data(), ws, period);
  else partial = sma(part.data(), ws, period);
  for (size_t i = 1; i <= std::min<size_t>(trail_count, ws); ++i)
    res[now_len - i] = partial[ws - i];

  cache.series[cache_key] = res;
  cache.series_ids[cache_key] = bar_ids;
  cache.last_len = now_len;
}

void update_ma_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                     const char* ma_type, int period, TaCache& cache) {
  update_ma_like_cache(bars_raw, cache_key, ma_type, period, cache,
                       [](const RawBar& b) { return b.close; }, kMaTrailCount);
}

void update_vol_ma_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                         const char* ma_type, int period, TaCache& cache) {
  update_ma_like_cache(bars_raw, cache_key, ma_type, period, cache,
                       [](const RawBar& b) { return b.vol; }, kMaVolTrailCount);
}

// ============================================================
// update_macd_cache（ta.rs:541-625）
// ============================================================
void update_macd_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                       int fast, int slow, int signal, TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);
  int min_count = signal + slow + kMacdMinCountExtra;

  bool full = !cache.macd.count(cache_key) || now_len < static_cast<size_t>(min_count + kFullRecomputeExtra);
  if (!full) {
    auto it = cache.macd.find(cache_key);
    if (it != cache.macd.end()) {
      if (now_len < 2 || it->second.ids.empty()) full = true;
      else {
        int32_t pen = bar_ids[now_len - 2];
        full = std::find(it->second.ids.begin(), it->second.ids.end(), pen) == it->second.ids.end();
      }
    }
  }

  std::vector<double> close(now_len);
  for (size_t i = 0; i < now_len; ++i) close[i] = bars_raw[i].close;

  if (full) {
    auto res = macd(close.data(), now_len, fast, slow, signal);
    res.ids = bar_ids;
    cache.macd[cache_key] = res;
    cache.last_len = now_len;
    return;
  }

  auto& existing = cache.macd[cache_key];
  std::unordered_map<int32_t, std::tuple<double,double,double>> old;
  for (size_t i = 0; i < existing.ids.size(); ++i)
    old[existing.ids[i]] = {existing.dif[i], existing.dea[i], existing.macd[i]};

  std::vector<double> dif(now_len, NAN), dea(now_len, NAN), macd_hist(now_len, NAN);
  for (size_t i = 0; i < now_len; ++i) {
    auto it = old.find(bar_ids[i]);
    if (it != old.end()) {
      dif[i] = std::get<0>(it->second);
      dea[i] = std::get<1>(it->second);
      macd_hist[i] = std::get<2>(it->second);
    }
  }

  size_t ws = std::min(static_cast<size_t>(min_count + kIncrementalWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> part(close.begin() + ws_start, close.end());
  auto partial = macd(part.data(), ws, fast, slow, signal);
  for (size_t i = 1; i <= std::min<size_t>(kMaTrailCount, ws); ++i) {
    dif[now_len - i] = partial.dif[ws - i];
    dea[now_len - i] = partial.dea[ws - i];
    macd_hist[now_len - i] = partial.macd[ws - i];
  }

  cache.macd[cache_key] = {bar_ids, dif, dea, macd_hist};
  cache.last_len = now_len;
}

// ============================================================
// update_boll_cache（ta.rs:640-742）
// ============================================================
void update_boll_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                       int period, double nbdev, TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);

  bool full = !cache.boll.count(cache_key) || !cache.boll_ids.count(cache_key)
              || now_len < static_cast<size_t>(period + kFullRecomputeExtra);
  if (!full) full = need_full_init(bar_ids, now_len, period, cache, cache.boll_ids, cache_key);

  std::vector<double> close(now_len);
  for (size_t i = 0; i < now_len; ++i) close[i] = bars_raw[i].close;

  if (full) {
    cache.boll[cache_key] = boll(close.data(), now_len, period, nbdev);
    cache.boll_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  auto& existing = cache.boll[cache_key];
  auto& ex_ids = cache.boll_ids[cache_key];
  std::unordered_map<int32_t, std::tuple<double,double,double>> old;
  for (size_t i = 0; i < ex_ids.size(); ++i)
    old[ex_ids[i]] = {existing.upper[i], existing.mid[i], existing.lower[i]};

  std::vector<double> upper(now_len, NAN), mid(now_len, NAN), lower(now_len, NAN);
  for (size_t i = 0; i < now_len; ++i) {
    auto it = old.find(bar_ids[i]);
    if (it != old.end()) {
      upper[i] = std::get<0>(it->second);
      mid[i] = std::get<1>(it->second);
      lower[i] = std::get<2>(it->second);
    }
  }

  size_t ws = std::min(static_cast<size_t>(period + kIncrementalWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> part(close.begin() + ws_start, close.end());
  auto partial = boll(part.data(), ws, period, nbdev);
  for (size_t i = 1; i <= std::min<size_t>(kMaTrailCount, ws); ++i) {
    upper[now_len - i] = partial.upper[ws - i];
    mid[now_len - i]   = partial.mid[ws - i];
    lower[now_len - i] = partial.lower[ws - i];
  }

  cache.boll[cache_key] = {upper, mid, lower};
  cache.boll_ids[cache_key] = bar_ids;
  cache.last_len = now_len;
}

// ============================================================
// update_kdj_cache（ta.rs:906-1027）
// ============================================================
void update_kdj_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int fastk, int slowk, int slowd, TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0 || fastk == 0 || slowk == 0 || slowd == 0) return;
  auto bar_ids = extract_ids(bars_raw);
  int min_count = fastk + slowk;

  bool full = !cache.kdj.count(cache_key) || now_len < static_cast<size_t>(min_count + kFullRecomputeExtra);
  if (!full) {
    auto it = cache.kdj.find(cache_key);
    if (it != cache.kdj.end()) {
      if (now_len < 2 || it->second.ids.empty()) full = true;
      else {
        int32_t pen = bar_ids[now_len - 2];
        full = std::find(it->second.ids.begin(), it->second.ids.end(), pen) == it->second.ids.end();
      }
    }
  }

  std::vector<double> high(now_len), low(now_len), close(now_len);
  for (size_t i = 0; i < now_len; ++i) {
    high[i] = bars_raw[i].high;
    low[i] = bars_raw[i].low;
    close[i] = bars_raw[i].close;
  }

  if (full) {
    auto res = stoch(high.data(), low.data(), close.data(), now_len, fastk, slowk, slowd);
    res.ids = bar_ids;
    cache.kdj[cache_key] = res;
    cache.last_len = now_len;
    return;
  }

  auto& existing = cache.kdj[cache_key];
  std::unordered_map<int32_t, std::tuple<double,double,double>> old;
  for (size_t i = 0; i < existing.ids.size(); ++i)
    old[existing.ids[i]] = {existing.k[i], existing.d[i], existing.j[i]};

  std::vector<double> k(now_len, NAN), d(now_len, NAN), j(now_len, NAN);
  for (size_t i = 0; i < now_len; ++i) {
    auto it = old.find(bar_ids[i]);
    if (it != old.end()) {
      k[i] = std::get<0>(it->second);
      d[i] = std::get<1>(it->second);
      j[i] = std::get<2>(it->second);
    }
  }

  size_t ws = std::min(static_cast<size_t>(min_count + kIncrementalWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> ph(high.begin() + ws_start, high.end());
  std::vector<double> pl(low.begin() + ws_start, low.end());
  std::vector<double> pc(close.begin() + ws_start, close.end());
  auto partial = stoch(ph.data(), pl.data(), pc.data(), ws, fastk, slowk, slowd);
  for (size_t i = 1; i <= std::min<size_t>(kMaTrailCount, ws); ++i) {
    k[now_len - i] = partial.k[ws - i];
    d[now_len - i] = partial.d[ws - i];
    j[now_len - i] = 3.0 * k[now_len - i] - 2.0 * d[now_len - i];
  }

  cache.kdj[cache_key] = {bar_ids, k, d, j};
  cache.last_len = now_len;
}

// ============================================================
// update_rsi_cache（ta.rs:1030-1088）
// ============================================================
void update_rsi_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int period, TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);

  if (!cache.series.count(cache_key) || !cache.series_ids.count(cache_key)) {
    std::vector<double> close(now_len);
    for (size_t i = 0; i < now_len; ++i) close[i] = bars_raw[i].close;
    cache.series[cache_key] = rsi(close.data(), now_len, period);
    cache.series_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  bool use_full = now_len < static_cast<size_t>(period + kFullRecomputeExtra) || now_len < 2
                  || !std::count(cache.series_ids[cache_key].begin(),
                                 cache.series_ids[cache_key].end(), bar_ids[now_len - 2]);
  std::vector<double> close(now_len);
  for (size_t i = 0; i < now_len; ++i) close[i] = bars_raw[i].close;

  if (use_full) {
    cache.series[cache_key] = rsi(close.data(), now_len, period);
    cache.series_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  auto res = align_series(bar_ids, cache.series_ids[cache_key], cache.series[cache_key]);
  size_t ws = std::min(static_cast<size_t>(period + kIncrementalWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> part(close.begin() + ws_start, close.end());
  auto partial = rsi(part.data(), ws, period);
  for (size_t i = 1; i <= std::min<size_t>(kMaTrailCount, ws); ++i)
    res[now_len - i] = partial[ws - i];

  cache.series[cache_key] = res;
  cache.series_ids[cache_key] = bar_ids;
  cache.last_len = now_len;
}

// ============================================================
// update_atr_cache（ta.rs:745-823）
//   注：ATR 增量策略与其他不同——填 NaN 而非固定 N 根，窗口使用 kAtrWindowExtra。
// ============================================================
void update_atr_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int period, TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);

  bool full = !cache.series.count(cache_key) || now_len < static_cast<size_t>(period + kFullRecomputeExtra);
  if (!full) full = need_full_init(bar_ids, now_len, period, cache, cache.series_ids, cache_key);

  std::vector<double> high(now_len), low(now_len), close(now_len);
  for (size_t i = 0; i < now_len; ++i) {
    high[i] = bars_raw[i].high;
    low[i] = bars_raw[i].low;
    close[i] = bars_raw[i].close;
  }

  if (full) {
    cache.series[cache_key] = atr(high.data(), low.data(), close.data(), now_len, period);
    cache.series_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  auto res = align_series(bar_ids, cache.series_ids[cache_key], cache.series[cache_key]);
  size_t ws = std::min(static_cast<size_t>(period + kAtrWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> ph(high.begin() + ws_start, high.end());
  std::vector<double> pl(low.begin() + ws_start, low.end());
  std::vector<double> pc(close.begin() + ws_start, close.end());
  auto partial = atr(ph.data(), pl.data(), pc.data(), ws, period);
  for (size_t i = 0; i < ws; ++i) {
    if (std::isnan(res[ws_start + i])) res[ws_start + i] = partial[i];
  }
  res[now_len - 1] = partial[ws - 1];

  cache.series[cache_key] = res;
  cache.series_ids[cache_key] = bar_ids;
  cache.last_len = now_len;
}

// ============================================================
// update_cci_cache（ta.rs:826-903）
//   注：CCI 增量策略同 ATR——填 NaN 而非固定 N 根。
// ============================================================
void update_cci_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int period, TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);

  bool full = !cache.series.count(cache_key) || now_len < static_cast<size_t>(period + kFullRecomputeExtra);
  if (!full) full = need_full_init(bar_ids, now_len, period, cache, cache.series_ids, cache_key);

  std::vector<double> high(now_len), low(now_len), close(now_len);
  for (size_t i = 0; i < now_len; ++i) {
    high[i] = bars_raw[i].high;
    low[i] = bars_raw[i].low;
    close[i] = bars_raw[i].close;
  }

  if (full) {
    cache.series[cache_key] = cci(high.data(), low.data(), close.data(), now_len, period);
    cache.series_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  auto res = align_series(bar_ids, cache.series_ids[cache_key], cache.series[cache_key]);
  size_t ws = std::min(static_cast<size_t>(period + kIncrementalWindowExtra), now_len);
  size_t ws_start = now_len - ws;
  std::vector<double> ph(high.begin() + ws_start, high.end());
  std::vector<double> pl(low.begin() + ws_start, low.end());
  std::vector<double> pc(close.begin() + ws_start, close.end());
  auto partial = cci(ph.data(), pl.data(), pc.data(), ws, period);
  for (size_t i = 0; i < ws; ++i) {
    if (std::isnan(res[ws_start + i])) res[ws_start + i] = partial[i];
  }
  res[now_len - 1] = partial[ws - 1];

  cache.series[cache_key] = res;
  cache.series_ids[cache_key] = bar_ids;
  cache.last_len = now_len;
}

// ============================================================
// update_sar_cache（ta.rs:1091-1154）
//   注：SAR 增量策略最特殊 —— 仅在「ID 不在旧缓存中」的位置覆盖，窗口固定上限。
// ============================================================
void update_sar_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      TaCache& cache) {
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  auto bar_ids = extract_ids(bars_raw);

  std::vector<double> high(now_len), low(now_len);
  for (size_t i = 0; i < now_len; ++i) {
    high[i] = bars_raw[i].high;
    low[i] = bars_raw[i].low;
  }

  if (!cache.series.count(cache_key) || !cache.series_ids.count(cache_key)) {
    cache.series[cache_key] = sar(high.data(), low.data(), now_len, kSarAccelInit, kSarAccelMax);
    cache.series_ids[cache_key] = bar_ids;
    cache.last_len = now_len;
    return;
  }

  bool use_full = now_len < kSarFullThreshold || now_len < 2
                  || !std::count(cache.series_ids[cache_key].begin(),
                                 cache.series_ids[cache_key].end(), bar_ids[now_len - 2]);
  size_t ws_start, ws;
  if (use_full) {
    ws_start = 0;
    ws = now_len;
  } else {
    ws = std::min<size_t>(kSarWindowMax, now_len);
    ws_start = now_len - ws;
  }

  std::vector<double> ph(high.begin() + ws_start, high.end());
  std::vector<double> pl(low.begin() + ws_start, low.end());
  auto partial = sar(ph.data(), pl.data(), ws, kSarAccelInit, kSarAccelMax);

  auto res = align_series(bar_ids, cache.series_ids[cache_key], cache.series[cache_key]);
  for (size_t i = 0; i < ws; ++i) {
    if (!std::count(cache.series_ids[cache_key].begin(),
                    cache.series_ids[cache_key].end(), bar_ids[ws_start + i]))
      res[ws_start + i] = partial[i];
  }
  res[now_len - 1] = partial[ws - 1];

  cache.series[cache_key] = res;
  cache.series_ids[cache_key] = bar_ids;
  cache.last_len = now_len;
}

// ============================================================
// MaSnapshotValue — 读取指定 RawBar 的 MA 值（对齐 utils/ta.rs:124）
//
// - 由 series_ids[cache_key] 找 bar id 的 index → 取 series[index]
// - 若 raw_bar.close 与 bars_raw[idx].close 一致 → 直接返回缓存
// - 不一致（同 dt 延伸阶段的历史快照）→ 用 calc_ma_cache_style 重算并把
//   idx 处 close 替换为 raw_bar.close，结果缓存到 snapshot_overrides 复用
// ============================================================
std::optional<double> MaSnapshotValue(const TaCache& cache, const std::string& cache_key,
                                      const RawBar& raw_bar, const std::string& ma_type,
                                      int period,
                                      std::unordered_map<int32_t, double>& overrides) {
  auto it_ids = cache.series_ids.find(cache_key);
  auto it_vals = cache.series.find(cache_key);
  if (it_ids == cache.series_ids.end() || it_vals == cache.series.end()) return std::nullopt;
  const auto& ids = it_ids->second;
  const auto& vals = it_vals->second;
  // 找 bar id 对应的 index（ids 与 vals 1:1 对齐）
  auto it = std::find(ids.begin(), ids.end(), raw_bar.id);
  if (it == ids.end()) return std::nullopt;
  size_t idx = static_cast<size_t>(std::distance(ids.begin(), it));
  if (idx >= vals.size()) return std::nullopt;

  // 同 dt 延伸时，raw_bar.close 为历史快照 close，与当前 bars_raw close 不同
  if (auto ov = overrides.find(raw_bar.id); ov != overrides.end()) return ov->second;

  // 无法取得当前 close 作比对时，直接返回缓存
  const double base = vals[idx];
  if (std::isnan(base)) return std::nullopt;
  return base;
}

// 带重算的快照：ma_type/period 已知时，override close 不一致则重算并缓存
std::optional<double> MaSnapshotValueWithRecompute(
    const std::vector<RawBar>& bars_raw, const TaCache& cache,
    const std::string& cache_key, const RawBar& raw_bar, const std::string& ma_type,
    int period, std::unordered_map<int32_t, double>& overrides) {
  auto it_ids = cache.series_ids.find(cache_key);
  if (it_ids == cache.series_ids.end()) return std::nullopt;
  const auto& ids = it_ids->second;
  auto it = std::find(ids.begin(), ids.end(), raw_bar.id);
  if (it == ids.end()) return std::nullopt;
  size_t idx = static_cast<size_t>(std::distance(ids.begin(), it));

  if (auto ov = overrides.find(raw_bar.id); ov != overrides.end()) return ov->second;

  // 比对当前 close；不一致则重算
  if (idx < bars_raw.size() && std::abs(bars_raw[idx].close - raw_bar.close) < 1e-9) {
    auto itv = cache.series.find(cache_key);
    if (itv == cache.series.end() || idx >= itv->second.size()) return std::nullopt;
    double base = itv->second[idx];
    if (std::isnan(base)) return std::nullopt;
    return base;
  }

  // 重算：取全部 close，把 idx 处替换为 raw_bar.close
  if (idx >= bars_raw.size()) return std::nullopt;
  std::vector<double> closes;
  closes.reserve(bars_raw.size());
  for (const auto& b : bars_raw) closes.push_back(b.close);
  closes[idx] = raw_bar.close;
  auto series = calc_ma_cache_style(closes, period, ma_type);
  if (idx >= series.size() || std::isnan(series[idx])) return std::nullopt;
  double value = series[idx];
  overrides[raw_bar.id] = value;
  return value;
}

// ============================================================
// MacdSnapshotValue — 读取指定 RawBar 的 MACD 字段（对齐 utils/ta.rs:68）
// field: 0=dif, 1=dea, 2=macd_hist
// ============================================================
std::optional<double> MacdSnapshotValue(
    const std::vector<RawBar>& bars_raw, const TaCache& cache,
    const std::string& cache_key, const RawBar& raw_bar,
    int fast, int slow, int signal, int field,
    std::unordered_map<int32_t, std::tuple<double,double,double>>& overrides) {
  auto it_m = cache.macd.find(cache_key);
  if (it_m == cache.macd.end()) return std::nullopt;
  const auto& series = it_m->second;  // MacdSeries，避免与 macd() 函数同名
  // 找 bar id 对应的 index
  auto it = std::find(series.ids.begin(), series.ids.end(), raw_bar.id);
  if (it == series.ids.end()) return std::nullopt;
  size_t idx = static_cast<size_t>(std::distance(series.ids.begin(), it));
  if (idx >= series.dif.size()) return std::nullopt;

  // 同 dt 延伸时，raw_bar.close 为历史快照 close
  if (auto ov = overrides.find(raw_bar.id); ov != overrides.end()) {
    const auto& [d, e, h] = ov->second;
    double vals[3] = {d, e, h};
    return (field >= 0 && field < 3) ? std::optional(vals[field]) : std::nullopt;
  }

  if (idx < bars_raw.size() && std::abs(bars_raw[idx].close - raw_bar.close) < 1e-9) {
    double vals[3] = {series.dif[idx], series.dea[idx], series.macd[idx]};
    if (std::isnan(vals[field])) return std::nullopt;
    return vals[field];
  }

  // 重算：替换 idx 处 close 后重算 MACD
  if (idx >= bars_raw.size()) return std::nullopt;
  std::vector<double> closes;
  closes.reserve(bars_raw.size());
  for (const auto& b : bars_raw) closes.push_back(b.close);
  closes[idx] = raw_bar.close;
  auto res = czsc::ta::macd(closes.data(), static_cast<int>(closes.size()), fast, slow, signal);
  if (idx >= res.dif.size()) return std::nullopt;
  double vals[3] = {res.dif[idx], res.dea[idx], res.macd[idx]};
  if (std::isnan(vals[field])) return std::nullopt;
  overrides[raw_bar.id] = {vals[0], vals[1], vals[2]};
  return vals[field];
}

// ============================================================
// update_obv_cache（对齐 Rust obv.rs update_obv_cache：OBV 累计量）
//   ta-lib TA_OBV(vol, close)，前 period 根为 NaN
// ============================================================
void update_obv_cache(const std::vector<RawBar>& bars_raw, TaCache& cache) {
  constexpr const char* kKey = "OBV";
  size_t now_len = bars_raw.size();
  if (now_len == 0) return;
  std::vector<double> vol(now_len), close(now_len);
  for (size_t i = 0; i < now_len; ++i) {
    vol[i] = static_cast<double>(bars_raw[i].vol);
    close[i] = bars_raw[i].close;
  }
  auto result = czsc::ta::obv(vol.data(), close.data(), static_cast<int>(now_len));
  cache.series[kKey] = std::move(result);
  cache.series_ids[kKey] = extract_ids(bars_raw);
  cache.last_len = now_len;
}

}  // namespace czsc::ta
