#include "tdx/proto/server_pool.hpp"

#include <algorithm>
#include <chrono>

#include <boost/asio/ip/address.hpp>

#include "util/fibers/fibers.h"
#include "util/fibers/synchronization.h"
#include "util/fiber_socket_base.h"

namespace tdx::proto {

ServerPool::ServerPool(::util::ProactorPool* pool) : pool_(pool) {}

std::optional<double> ServerPool::Probe(const ServerInfo& host,
                                        ::util::fb2::ProactorBase* pb) {
  // 对齐 mootdx server.py connect：socket connect 测延迟（这里用 helio FiberSocket）。
  boost::system::error_code addr_ec;
  auto addr = boost::asio::ip::make_address(host.ip, addr_ec);
  if (addr_ec) return std::nullopt;  // 非法 IP

  ::util::FiberSocketBase::endpoint_type ep(addr, host.port);
  std::unique_ptr<::util::FiberSocketBase> sock(pb->CreateSocket());
  sock->set_timeout(2000);  // 2s 超时（对齐 mootdx socket.settimeout，避免不可达 host 挂起）

  auto start = std::chrono::steady_clock::now();
  auto conn_ec = sock->Connect(ep);
  double ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start).count();
  sock->Close();
  if (conn_ec) return std::nullopt;  // 连接失败
  return ms;
}

std::optional<ServerInfo> ServerPool::SelectBest(const std::vector<ServerInfo>& hosts) {
  if (hosts.empty()) return std::nullopt;

  struct ProbeResult {
    ServerInfo host;
    double latency_ms;
  };

  std::vector<ProbeResult> results;
  ::util::fb2::Mutex mu;  // 保护 results
  std::vector<::util::fb2::Fiber> fibers;
  fibers.reserve(hosts.size());

  // 每个 host 一个 fiber 并发测速（PATTERN 2，echo_server TLocalClient::Connect:580-596）
  for (const auto& host : hosts) {
    fibers.push_back(::util::MakeFiber([&, host] {
      auto* pb = pool_->GetNextProactor();
      auto lat = pb->Await([&] { return Probe(host, pb); });
      if (lat) {
        std::lock_guard<::util::fb2::Mutex> lk(mu);
        results.push_back({host, *lat});
      }
    }));
  }
  for (auto& f : fibers) f.Join();  // 收集全部测速结果

  if (results.empty()) return std::nullopt;
  auto best = std::min_element(results.begin(), results.end(),
                               [](const ProbeResult& a, const ProbeResult& b) {
                                 return a.latency_ms < b.latency_ms;
                               });
  return best->host;
}

}  // namespace tdx::proto
