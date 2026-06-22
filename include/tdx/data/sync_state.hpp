// 增量同步状态。对齐 tdxdata/sync.py:16-65。
// JSON 持久化 schema: {stock_code: {data_type: {last_sync, updated_at}}}。
// 默认 ~/.tdx-cpp/sync_state.json。
#pragma once

#include <map>
#include <string>

namespace tdx::data {

struct SyncEntry {
  std::string last_sync;   // YYYY-MM-DD
  std::string updated_at;  // ISO8601
};

class SyncState {
 public:
  SyncState();
  explicit SyncState(std::string path);

  void Load();
  void Save() const;

  std::string GetLastSync(const std::string& stock, const std::string& data_type) const;
  void UpdateSync(const std::string& stock, const std::string& data_type,
                  const std::string& last_sync = "");
  void Clear();

  const std::string& path() const { return path_; }

 private:
  std::string path_;
  // {stock: {data_type: SyncEntry}}
  std::map<std::string, std::map<std::string, SyncEntry>> state_;
};

}  // namespace tdx::data
