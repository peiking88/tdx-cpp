// 增量同步状态 + 股票级断点续传。对齐 tdxdata/sync.py:16-65 + Phase 4 增强。
// JSON schema: {stock_code: {data_type: {last_sync, updated_at, batch_id, batch_status, retry_count}}}。
// 默认 ~/.tdx-cpp/sync_state.json。多 fiber 并发安全（fb2::Mutex）。
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "util/fibers/synchronization.h"  // fb2::Mutex（nlohmann::json 非线程安全）

namespace tdx::data {

struct SyncEntry {
  std::string last_sync;    // YYYY-MM-DD
  std::string updated_at;   // ISO8601
  // Phase 4 断点续传字段（向后兼容：旧 schema 缺这些字段默认空）
  std::string batch_id;     // 本批次唯一 ID
  std::string batch_status; // pending/in_progress/completed/failed
  int retry_count = 0;
  std::string last_error;   // 诊断用
};

class SyncState {
 public:
  SyncState();
  explicit SyncState(std::string path);

  void Load();
  void Save();

  std::string GetLastSync(const std::string& stock, const std::string& data_type) const;
  void UpdateSync(const std::string& stock, const std::string& data_type,
                  const std::string& last_sync = "");
  void Clear();

  // ---- Phase 4 股票级断点续传 ----
  // 启动批次（记录 batch_id，便于崩溃恢复查找）
  void StartBatch(const std::string& batch_id);
  // 获取当前活跃批次 ID（空字符串 = 无活跃批次）
  std::string GetActiveBatch() const;
  // 该股票在本批次是否已完成（崩溃恢复跳过）
  bool IsCompletedInBatch(const std::string& stock, const std::string& data_type,
                          const std::string& batch_id) const;
  // 标记单股票完成
  void MarkStockComplete(const std::string& stock, const std::string& data_type,
                         const std::string& batch_id);
  // 标记单股票失败（retry_count++）
  void MarkStockFailed(const std::string& stock, const std::string& data_type,
                       const std::string& batch_id, std::string_view err);

  const std::string& path() const { return path_; }

 private:
  std::string path_;
  mutable ::util::fb2::Mutex mu_;  // 保护 state_+batch_id_（多 fiber 并发写）
  std::map<std::string, std::map<std::string, SyncEntry>> state_;
  std::string batch_id_;           // 当前活跃批次 ID
};

}  // namespace tdx::data
