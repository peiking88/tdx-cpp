// 自动选服。对齐 mootdx/server.py（并发 socket connect 测延迟，选最快）
// + mootdx/config.py（config.json 缓存最优 IP）。
// 用 helio MakeFiber 并发测速（PATTERN 2，echo_server TLocalClient::Connect）。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "util/fibers/pool.h"
#include "util/fibers/proactor_base.h"

namespace tdx::proto {

// 服务器信息（对齐 mootdx hosts 项）
struct ServerInfo {
  std::string name;
  std::string ip;
  uint16_t port = 0;
};

class ServerPool {
 public:
  explicit ServerPool(::util::ProactorPool* pool);

  // 并发测速所有 hosts（每个一个 fiber），返回延迟最低者；全部不可达返回 nullopt。
  std::optional<ServerInfo> SelectBest(const std::vector<ServerInfo>& hosts);

  // 测速单个 host 的 TCP connect 延迟（ms）；失败返回 nullopt。须在 fiber 内调用。
  static std::optional<double> Probe(const ServerInfo& host,
                                     ::util::fb2::ProactorBase* pb);

 private:
  ::util::ProactorPool* pool_;
};

}  // namespace tdx::proto
