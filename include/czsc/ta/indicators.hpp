// TA 指标函数声明（thin wrappers over ta-lib C API）
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "czsc/ta/ta_cache.hpp"
#include "czsc/types/raw_bar.hpp"

namespace czsc::ta {

// ============================================================
// Thin wrappers — 每个函数直接调用 ta-lib TA_XXX，填充 NaN 前缀
// ============================================================

// EMA 指数移动平均
std::vector<double> ema(const double* close, size_t len, int period);

// SMA 简单移动平均
std::vector<double> sma(const double* close, size_t len, int period);

// WMA 加权移动平均
std::vector<double> wma(const double* close, size_t len, int period);

// MACD {dif, dea, macd}
MacdSeries macd(const double* close, size_t len, int fast, int slow, int signal);

// RSI (Wilder's smoothing)
std::vector<double> rsi(const double* close, size_t len, int period);

// ATR 平均真实波幅
std::vector<double> atr(const double* high, const double* low, const double* close,
                        size_t len, int period);

// CCI 商品通道指数
std::vector<double> cci(const double* high, const double* low, const double* close,
                        size_t len, int period);

// BOLL 布林带 — 返回 (upper, mid, lower)
BollSeries boll(const double* close, size_t len, int period, double nbdev);

// KDJ 随机指标 — 返回 (slowK, slowD)
KdjSeries stoch(const double* high, const double* low, const double* close,
                size_t len, int fastk, int slowk, int slowd);

// SAR 抛物线转向
std::vector<double> sar(const double* high, const double* low, size_t len,
                        double acceleration, double maximum);

// OBV 能量潮
std::vector<double> obv(const double* vol, const double* close, size_t len);

// ============================================================
// 缓存键生成
// ============================================================
inline std::string ma_cache_key(const char* type, int period) {
  return std::string(type) + "_" + std::to_string(period);
}
inline std::string macd_cache_key(int s, int l, int m) {
  return "macd_" + std::to_string(s) + "_" + std::to_string(l) + "_" + std::to_string(m);
}
inline std::string boll_cache_key(int period, double nbdev) {
  char buf[48];
  snprintf(buf, sizeof(buf), "boll_%d_%.1f", period, nbdev);
  return buf;
}
inline std::string kdj_cache_key(int fk, int sk, int sd) {
  return "kdj_" + std::to_string(fk) + "_" + std::to_string(sk) + "_" + std::to_string(sd);
}

// ============================================================
// 缓存风格 MA 计算（对齐 Rust calc_*_cache_style：TA-Lib 风格 NaN 前缀）
// 用于 cxt_second_bs / cxt_third_bs 的均线快照
// ============================================================
// SMA 缓存风格：前 period-1 个 NaN，之后定长滑动均值（先减旧值再加新值）
std::vector<double> calc_sma_cache_style(const std::vector<double>& series, int period);
// EMA 缓存风格：对齐 TA-Lib EMA（前 period-1 个 NaN）
std::vector<double> calc_ema_cache_style(const std::vector<double>& series, int period);
// WMA 缓存风格：前 period-1 个 NaN，之后定长加权均值
std::vector<double> calc_wma_cache_style(const std::vector<double>& series, int period);
// 按 ma_type 分派（"SMA"/"EMA"/"WMA"，大小写不敏感，默认 SMA）
std::vector<double> calc_ma_cache_style(const std::vector<double>& series, int period,
                                        const std::string& ma_type);

// ============================================================
// 增量缓存更新函数（对齐 Rust update_*_cache 语义）
// ============================================================
void update_ma_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                     const char* ma_type, int period, TaCache& cache);

void update_vol_ma_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                         const char* ma_type, int period, TaCache& cache);

void update_macd_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                       int fast, int slow, int signal, TaCache& cache);

void update_boll_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                       int period, double nbdev, TaCache& cache);

void update_kdj_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int fastk, int slowk, int slowd, TaCache& cache);

void update_rsi_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int period, TaCache& cache);

void update_atr_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int period, TaCache& cache);

void update_cci_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      int period, TaCache& cache);

void update_sar_cache(const std::vector<RawBar>& bars_raw, const char* cache_key,
                      TaCache& cache);

// OBV 累计量序列（对齐 Rust obv.rs update_obv_cache："OBV" series_ids=bar.id 序列）
//   NaN 前缀 (period-1=0 所以无), ta-lib OBV(vol, close)
void update_obv_cache(const std::vector<RawBar>& bars_raw, TaCache& cache);

}  // namespace czsc::ta
