// SP/MAC 高级行情（mac_hosts，板块/资金流/异动）。组合标准 Connection（head=0x0c）+ mac_hosts。
// 与标准行情差异：连 mac_hosts（端口 7709，IP 不同），SP 业务靠 msg_id 0x1215-0x123E 区分。
// 登录复用标准 0x0d，心跳复用 0x04。
#pragma once

#include <memory>
#include <system_error>
#include <vector>

#include "util/fibers/pool.h"
#include "util/fibers/proactor_base.h"

#include "tdx/proto/connection.hpp"
#include "tdx/proto/heartbeat.hpp"
#include "tdx/proto/retry.hpp"
#include "tdx/proto/server_pool.hpp"

namespace tdx::quotes {

class SPQuotes {
 public:
  SPQuotes();
  ~SPQuotes();

  SPQuotes(const SPQuotes&) = delete;
  SPQuotes& operator=(const SPQuotes&) = delete;

  // 启动：ProactorPool + mac_hosts 选服 + 连接 + 标准登录 0x0d + 心跳。
  std::error_code Connect();
  void Close();
  bool IsConnected() const { return connected_; }

 protected:
  // 供 SP API（Task 10-12 的 BoardList/Unusual/CapitalFlow 等）复用的请求入口。
  proto::Response Call(uint16_t msg_id, const std::vector<uint8_t>& body);
  ::util::fb2::ProactorBase* proactor() { return proactor_; }

 private:
  std::error_code ConnectInFiber();
  void SendHeartbeat();
  static std::vector<proto::ServerInfo> DefaultMacHosts();

  std::unique_ptr<::util::ProactorPool> pool_;
  ::util::fb2::ProactorBase* proactor_ = nullptr;
  std::unique_ptr<proto::Connection> conn_;
  std::unique_ptr<proto::Heartbeat> heartbeat_;
  std::unique_ptr<proto::ServerPool> server_pool_;
  proto::RetryPolicy retry_;
  proto::CircuitBreaker breaker_;
  bool connected_ = false;
};

}  // namespace tdx::quotes
