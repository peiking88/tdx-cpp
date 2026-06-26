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
                                         const std::string& dividend,
                                         const std::string& batch_id) {
  (void)start;
  (void)end;
  (void)dividend;  // TODO: 复权需 xdxr 事件流，v1 暂忽略

  tdx::Period p = tdx::Period::DAILY;
  if (period == "5m") p = tdx::Period::MIN_5;
  else if (period == "1m") p = tdx::Period::MIN_1;
  else if (period == "15m") p = tdx::Period::MIN_15;
  else if (period == "30m") p = tdx::Period::MIN_30;
  else if (period == "1h") p = tdx::Period::HOUR_1;

  std::vector<KLine> all;
  for (const auto& code : codes) {
    // T4 断点续传：batch_id 非空且已完成 → 跳过（--resume 崩溃恢复）
    if (!batch_id.empty() && sync_.IsCompletedInBatch(code, "history_kline", batch_id)) {
      continue;
    }
    auto bars = sq_.Bars(tdx::MarketFromCode(code), code, p, 0, tdx::kKlineMaxCount);
    // T4 标记完成（batch 模式 MarkStockComplete，否则 UpdateSync）
    if (!batch_id.empty()) {
      sync_.MarkStockComplete(code, "history_kline", batch_id);
    } else {
      sync_.UpdateSync(code, "history_kline");
    }
    for (auto& b : bars) all.push_back(std::move(b));
  }
  return all;
}

std::string TdxData::SyncStatus(const std::string& code, const std::string& data_type) {
  return sync_.GetLastSync(code, data_type);
}

}  // namespace tdx::data
