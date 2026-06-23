// SyncState 实现（Phase 4 增强：fb2::Mutex 并发安全 + std::filesystem + 批次断点续传）。
#include "tdx/data/sync_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace tdx::data {
namespace {

std::string DefaultPath() {
  const char* home = std::getenv("HOME");
  std::string h = home ? home : "/tmp";
  return h + "/.tdx-cpp/sync_state.json";
}

std::string FormatTime(const char* fmt) {
  std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_r(&t, &lt);
  char b[32];
  std::strftime(b, sizeof(b), fmt, &lt);
  return std::string(b);
}

void EnsureDir(const std::string& path) {
  // C++17 std::filesystem（替代 std::system，避免 fork+exec 阻塞 Proactor 线程）
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return;
  std::error_code ec;
  std::filesystem::create_directories(path.substr(0, pos), ec);
}

// 写 state_ 到 JSON 文件（调用者已加锁）
void WriteState(const std::string& path,
                const std::map<std::string, std::map<std::string, SyncEntry>>& state) {
  EnsureDir(path);
  nlohmann::json j;
  for (const auto& [stock, types] : state) {
    for (const auto& [dt, e] : types) {
      j[stock][dt] = {{"last_sync", e.last_sync},
                      {"updated_at", e.updated_at},
                      {"batch_id", e.batch_id},
                      {"batch_status", e.batch_status},
                      {"retry_count", e.retry_count},
                      {"last_error", e.last_error}};
    }
  }
  std::ofstream f(path);
  if (f) f << j.dump(2);
}

}  // namespace

SyncState::SyncState() : path_(DefaultPath()) {}
SyncState::SyncState(std::string path) : path_(std::move(path)) {}

void SyncState::Load() {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  std::ifstream f(path_);
  if (!f) return;
  try {
    auto j = nlohmann::json::parse(f);
    for (auto& [stock, types] : j.items()) {
      if (!types.is_object()) continue;
      for (auto& [dt, entry] : types.items()) {
        SyncEntry e;
        e.last_sync = entry.value("last_sync", "");
        e.updated_at = entry.value("updated_at", "");
        e.batch_id = entry.value("batch_id", "");
        e.batch_status = entry.value("batch_status", "");
        e.retry_count = entry.value("retry_count", 0);
        e.last_error = entry.value("last_error", "");
        state_[stock][dt] = e;
      }
    }
  } catch (...) {
  }
}

void SyncState::Save() {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  WriteState(path_, state_);
}

std::string SyncState::GetLastSync(const std::string& stock, const std::string& data_type) const {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  auto it = state_.find(stock);
  if (it == state_.end()) return "";
  auto it2 = it->second.find(data_type);
  if (it2 == it->second.end()) return "";
  return it2->second.last_sync;
}

void SyncState::UpdateSync(const std::string& stock, const std::string& data_type,
                           const std::string& last_sync) {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  SyncEntry& e = state_[stock][data_type];
  e.last_sync = last_sync.empty() ? FormatTime("%Y-%m-%d") : last_sync;
  e.updated_at = FormatTime("%Y-%m-%dT%H:%M:%S");
  WriteState(path_, state_);
}

void SyncState::Clear() {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  state_.clear();
  std::error_code ec;
  std::filesystem::remove(path_, ec);
}

// ---- Phase 4 股票级断点续传 ----

void SyncState::StartBatch(const std::string& batch_id) {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  // 批次启动：不预置（MarkStockComplete 时记 batch_id）
  (void)batch_id;
}

bool SyncState::IsCompletedInBatch(const std::string& stock, const std::string& data_type,
                                   const std::string& batch_id) const {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  auto it = state_.find(stock);
  if (it == state_.end()) return false;
  auto it2 = it->second.find(data_type);
  if (it2 == it->second.end()) return false;
  const SyncEntry& e = it2->second;
  return e.batch_id == batch_id && e.batch_status == "completed";
}

void SyncState::MarkStockComplete(const std::string& stock, const std::string& data_type,
                                  const std::string& batch_id) {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  SyncEntry& e = state_[stock][data_type];
  e.batch_id = batch_id;
  e.batch_status = "completed";
  e.last_sync = FormatTime("%Y-%m-%d");
  e.updated_at = FormatTime("%Y-%m-%dT%H:%M:%S");
  WriteState(path_, state_);
}

void SyncState::MarkStockFailed(const std::string& stock, const std::string& data_type,
                                const std::string& batch_id, std::string_view err) {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  SyncEntry& e = state_[stock][data_type];
  e.batch_id = batch_id;
  e.batch_status = "failed";
  e.retry_count += 1;
  e.last_error = std::string(err);
  e.updated_at = FormatTime("%Y-%m-%dT%H:%M:%S");
  WriteState(path_, state_);
}

}  // namespace tdx::data
