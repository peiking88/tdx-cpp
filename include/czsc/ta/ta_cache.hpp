// TaCache 缓存数据结构
// 直译自 ~/peiking88/czsc/crates/czsc-signals/src/types.rs:16-61
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace czsc {
struct RawBar;  // 前置声明
}

namespace czsc::ta {

// MACD 缓存三元组（types.rs:6-14）
struct MacdSeries {
  std::vector<int32_t> ids;
  std::vector<double> dif;
  std::vector<double> dea;
  std::vector<double> macd;
};

// BOLL 缓存三元组（types.rs:17-22）
struct BollSeries {
  std::vector<double> upper;
  std::vector<double> mid;
  std::vector<double> lower;
};

// KDJ 缓存三元组（types.rs:25-31）
struct KdjSeries {
  std::vector<int32_t> ids;
  std::vector<double> k;
  std::vector<double> d;
  std::vector<double> j;
};

// TA 指标增量缓存（types.rs:34-55）
struct TaCache {
  // 简单单点序列（EMA/SMA/RSI/ATR/CCI/SAR）的缓存
  std::unordered_map<std::string, std::vector<double>> series;

  // MACD 数据缓存
  std::unordered_map<std::string, MacdSeries> macd;

  // BOLL 数据缓存
  std::unordered_map<std::string, BollSeries> boll;
  std::unordered_map<std::string, std::vector<int32_t>> boll_ids;

  // KDJ 数据缓存
  std::unordered_map<std::string, KdjSeries> kdj;

  // series 对应的 bar id 序列（用于 bars_raw 截断后对齐）
  std::unordered_map<std::string, std::vector<int32_t>> series_ids;

  // 标记已缓存的最大长度，用于增量判断
  size_t last_len = 0;
};

// ============================================================
// MaSnapshotValue — 读取指定 RawBar 的 MA 值（对齐 utils/ta.rs:124）
// ============================================================
// 基础快照：由缓存 series + series_ids 按 bar id 取值（假设 close 未发生变化）
std::optional<double> MaSnapshotValue(
    const TaCache& cache, const std::string& cache_key, const RawBar& raw_bar,
    const std::string& ma_type, int period,
    std::unordered_map<int32_t, double>& overrides);

// 带重算的快照：close 不一致时按 idx 重算（同 dt 延伸历史快照）
std::optional<double> MaSnapshotValueWithRecompute(
    const std::vector<RawBar>& bars_raw, const TaCache& cache,
    const std::string& cache_key, const RawBar& raw_bar, const std::string& ma_type,
    int period, std::unordered_map<int32_t, double>& overrides);

// MACD 快照：field 0=dif 1=dea 2=macd_hist（对齐 utils/ta.rs:68）
std::optional<double> MacdSnapshotValue(
    const std::vector<RawBar>& bars_raw, const TaCache& cache,
    const std::string& cache_key, const RawBar& raw_bar,
    int fast, int slow, int signal, int field,
    std::unordered_map<int32_t, std::tuple<double,double,double>>& overrides);

}  // namespace czsc::ta
