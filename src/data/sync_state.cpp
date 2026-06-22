// SyncState 实现。对齐 tdxdata/sync.py。
#include "tdx/data/sync_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sys/stat.h>

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
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return;
  std::string dir = path.substr(0, pos);
  // 递归 mkdir（简化：system mkdir -p）
  std::string cmd = "mkdir -p '" + dir + "'";
  std::system(cmd.c_str());
}

}  // namespace

SyncState::SyncState() : path_(DefaultPath()) {}
SyncState::SyncState(std::string path) : path_(std::move(path)) {}

void SyncState::Load() {
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
        state_[stock][dt] = e;
      }
    }
  } catch (...) {
    // 解析失败静默降级
  }
}

void SyncState::Save() const {
  EnsureDir(path_);
  nlohmann::json j;
  for (const auto& [stock, types] : state_) {
    for (const auto& [dt, e] : types) {
      j[stock][dt]["last_sync"] = e.last_sync;
      j[stock][dt]["updated_at"] = e.updated_at;
    }
  }
  std::ofstream f(path_);
  if (f) f << j.dump(2);
}

std::string SyncState::GetLastSync(const std::string& stock, const std::string& data_type) const {
  auto it = state_.find(stock);
  if (it == state_.end()) return "";
  auto it2 = it->second.find(data_type);
  if (it2 == it->second.end()) return "";
  return it2->second.last_sync;
}

void SyncState::UpdateSync(const std::string& stock, const std::string& data_type,
                           const std::string& last_sync) {
  SyncEntry e;
  e.last_sync = last_sync.empty() ? FormatTime("%Y-%m-%d") : last_sync;
  e.updated_at = FormatTime("%Y-%m-%dT%H:%M:%S");
  state_[stock][data_type] = e;
  Save();
}

void SyncState::Clear() {
  state_.clear();
  std::remove(path_.c_str());
}

}  // namespace tdx::data
