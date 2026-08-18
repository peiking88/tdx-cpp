// ta-lib thin wrappers
#include "czsc/ta/indicators.hpp"

#include <ta-lib/ta_func.h>

#include <cmath>
#include <cstring>

namespace czsc::ta {
namespace {

// 通用：分配 length 大小的 vector，前 outBegIdx 填 NaN，后段复制有效值
std::vector<double> fill_output(int outBegIdx, int outNbElement,
                                const double* out, size_t length) {
  std::vector<double> result(length, NAN);
  for (int i = 0; i < outNbElement; ++i)
    result[outBegIdx + i] = out[i];
  return result;
}

}  // namespace

std::vector<double> ema(const double* close, size_t len, int period) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_EMA(0, static_cast<int>(len) - 1, close, period, &outBeg, &outNb, out.data());
  std::vector<double> result(len, NAN);
  for (int i = 0; i < outNb; ++i) result[outBeg + i] = out[i];
  return result;
}

std::vector<double> sma(const double* close, size_t len, int period) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_SMA(0, static_cast<int>(len) - 1, close, period, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

std::vector<double> wma(const double* close, size_t len, int period) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_WMA(0, static_cast<int>(len) - 1, close, period, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

MacdSeries macd(const double* close, size_t len, int fast, int slow, int signal) {
  std::vector<double> dif(len), dea(len), hist(len);
  int outBeg = 0, outNb = 0;
  TA_MACD(0, static_cast<int>(len) - 1, close, fast, slow, signal,
          &outBeg, &outNb, dif.data(), dea.data(), hist.data());
  MacdSeries result;
  result.dif = fill_output(outBeg, outNb, dif.data(), len);
  result.dea = fill_output(outBeg, outNb, dea.data(), len);
  result.macd = fill_output(outBeg, outNb, hist.data(), len);
  return result;
}

std::vector<double> rsi(const double* close, size_t len, int period) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_RSI(0, static_cast<int>(len) - 1, close, period, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

std::vector<double> atr(const double* high, const double* low, const double* close,
                        size_t len, int period) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_ATR(0, static_cast<int>(len) - 1, high, low, close,
         period, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

std::vector<double> cci(const double* high, const double* low, const double* close,
                        size_t len, int period) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_CCI(0, static_cast<int>(len) - 1, high, low, close,
         period, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

BollSeries boll(const double* close, size_t len, int period, double nbdev) {
  std::vector<double> upper(len), mid(len), lower(len);
  int outBeg = 0, outNb = 0;
  TA_BBANDS(0, static_cast<int>(len) - 1, close, period,
            nbdev, nbdev, TA_MAType_SMA,
            &outBeg, &outNb, upper.data(), mid.data(), lower.data());
  BollSeries result;
  result.upper = fill_output(outBeg, outNb, upper.data(), len);
  result.mid   = fill_output(outBeg, outNb, mid.data(), len);
  result.lower = fill_output(outBeg, outNb, lower.data(), len);
  return result;
}

KdjSeries stoch(const double* high, const double* low, const double* close,
                size_t len, int fastk, int slowk, int slowd) {
  std::vector<double> outK(len), outD(len);
  int outBeg = 0, outNb = 0;
  TA_STOCH(0, static_cast<int>(len) - 1, high, low, close,
           fastk, slowk, TA_MAType_SMA, slowd, TA_MAType_SMA,
           &outBeg, &outNb, outK.data(), outD.data());
  KdjSeries result;
  result.k = fill_output(outBeg, outNb, outK.data(), len);
  result.d = fill_output(outBeg, outNb, outD.data(), len);
  // J = 3*K - 2*D
  result.j.resize(len, NAN);
  for (int i = outBeg; i < outBeg + outNb; ++i)
    result.j[i] = 3.0 * result.k[i] - 2.0 * result.d[i];
  return result;
}

std::vector<double> sar(const double* high, const double* low, size_t len,
                        double acceleration, double maximum) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_SAR(0, static_cast<int>(len) - 1, high, low,
         acceleration, maximum, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

// OBV（ta-lib TA_OBV(vol, close)）— 对齐 Rust utils/ta.rs OBVCacher
std::vector<double> obv(const double* vol, const double* close, size_t len) {
  std::vector<double> out(len);
  int outBeg = 0, outNb = 0;
  TA_OBV(0, static_cast<int>(len) - 1, vol, close, &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), len);
}

// ============================================================
// 缓存风格 MA 计算（对齐 Rust calc_*_cache_style：TA-Lib 风格 NaN 前缀）
// 用于 cxt_second_bs / cxt_third_bs 的均线快照（MaSnapshotValue）
// ============================================================
std::vector<double> calc_sma_cache_style(const std::vector<double>& series, int period) {
  const int len = static_cast<int>(series.size());
  std::vector<double> out(len, NAN);
  if (len < period || period <= 0) return out;
  double sum = 0.0;
  for (int i = 0; i < period; ++i) sum += series[i];
  out[period - 1] = sum / static_cast<double>(period);
  for (int i = period; i < len; ++i) {
    // 先减旧值再加新值，对齐 TA-Lib 累计顺序
    sum -= series[i - period];
    sum += series[i];
    out[i] = sum / static_cast<double>(period);
  }
  return out;
}

std::vector<double> calc_ema_cache_style(const std::vector<double>& series, int period) {
  // 与现有 ema() 封装一致：对齐 TA-Lib EMA（前 period-1 个 NaN）
  if (series.empty() || period <= 0) return {};
  std::vector<double> out(series.size());
  std::vector<double> closing(series.begin(), series.end());
  int outBeg = 0, outNb = 0;
  TA_EMA(0, static_cast<int>(closing.size()) - 1, closing.data(), period,
         &outBeg, &outNb, out.data());
  return fill_output(outBeg, outNb, out.data(), closing.size());
}

std::vector<double> calc_wma_cache_style(const std::vector<double>& series, int period) {
  const int len = static_cast<int>(series.size());
  std::vector<double> out(len, NAN);
  if (len < period || period <= 0) return out;
  const double denom = static_cast<double>(period * (period + 1) / 2);
  for (int i = period - 1; i < len; ++i) {
    double weighted = 0.0;
    for (int j = 0; j < period; ++j)
      weighted += series[i + 1 - period + j] * static_cast<double>(j + 1);
    out[i] = weighted / denom;
  }
  return out;
}

std::vector<double> calc_ma_cache_style(const std::vector<double>& series, int period,
                                        const std::string& ma_type) {
  if (ma_type == "EMA" || ma_type == "ema") return calc_ema_cache_style(series, period);
  if (ma_type == "WMA" || ma_type == "wma") return calc_wma_cache_style(series, period);
  return calc_sma_cache_style(series, period);  // 默认 SMA（大小写不敏感）
}

}  // namespace czsc::ta
