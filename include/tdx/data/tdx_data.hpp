// TdxData 统一 API（对齐 tdxdata/api.py）。组合 StdQuotes + Calendar + SyncState。
// 简化决策（PRD P2-2）：不实现独立 DataManager/Sources 虚函数插件，TdxData 直接编排。
// 注意：复权（dividend）需 xdxr 事件流，StdQuotes.Xdxr 尚未实现（Phase 1 TODO），
//       v1 FetchHistory 暂不应用复权（dividend 参数记录但忽略），待 xdxr 落地后补 ApplyAdjust。
#pragma once

#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "tdx/data/calendar.hpp"
#include "tdx/data/sync_state.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/types.hpp"

namespace tdx::data {

class TdxData {
 public:
  TdxData();
  ~TdxData();

  TdxData(const TdxData&) = delete;
  TdxData& operator=(const TdxData&) = delete;

  // 启动（StdQuotes 选服 + 连接 + 登录 + 心跳）
  std::error_code Connect();
  void Close();
  bool IsConnected() const { return sq_.IsConnected(); }

  // fetch_history（对齐 api.py:47）。codes 股票列表，period 周期（1d/5m/1m），dividend 复权（v1 暂忽略）。
  std::vector<KLine> FetchHistory(const std::vector<std::string>& codes,
                                  const std::string& start, const std::string& end,
                                  const std::string& period = "1d",
                                  const std::string& dividend = "none");

  // 增量同步状态查询（对齐 api.py:204）
  std::string SyncStatus(const std::string& code,
                         const std::string& data_type = "history_kline");

 private:
  quotes::StdQuotes sq_;
  Calendar cal_;
  SyncState sync_;
};

}  // namespace tdx::data
