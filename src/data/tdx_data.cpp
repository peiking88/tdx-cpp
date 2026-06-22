// TdxData 实现。组合 StdQuotes + SyncState，提供 fetch_history 统一 API。
#include "tdx/data/tdx_data.hpp"

namespace tdx::data {

TdxData::TdxData() { sync_.Load(); }
TdxData::~TdxData() { Close(); }

std::error_code TdxData::Connect() { return sq_.Connect(); }
void TdxData::Close() { sq_.Close(); }

std::vector<KLine> TdxData::FetchHistory(const std::vector<std::string>& codes,
                                         const std::string& start,
                                         const std::string& end,
                                         const std::string& period,
                                         const std::string& dividend) {
  (void)start;
  (void)end;
  (void)dividend;  // TODO: 复权需 xdxr 事件流（StdQuotes.Xdxr 未实现），v1 暂忽略

  // period 字符串 → Period 枚举
  tdx::Period p = tdx::Period::DAILY;
  if (period == "5m") p = tdx::Period::MIN_5;
  else if (period == "1m") p = tdx::Period::MIN_1;
  else if (period == "15m") p = tdx::Period::MIN_15;
  else if (period == "30m") p = tdx::Period::MIN_30;
  else if (period == "1h") p = tdx::Period::HOUR_1;

  std::vector<KLine> all;
  for (const auto& code : codes) {
    auto bars = sq_.Bars(tdx::MarketFromCode(code), code, p, 0, tdx::kKlineMaxCount);
    sync_.UpdateSync(code, "history_kline");  // 增量同步状态更新
    for (auto& b : bars) all.push_back(std::move(b));
  }
  return all;
}

std::string TdxData::SyncStatus(const std::string& code, const std::string& data_type) {
  return sync_.GetLastSync(code, data_type);
}

}  // namespace tdx::data
