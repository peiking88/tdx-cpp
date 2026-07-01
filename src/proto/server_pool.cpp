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

  // 单调度器并行（batch_fetch 模式）：所有探测 fiber 共用同一 proactor，
  // socket 同属该调度器，io_uring/epoll 单线程多路复用 N 个 socket。
  // 修复跨调度器违规：原实现 MakeFiber 后调 GetNextProactor()->Await 造成
  // fiber 与 pb 分属不同调度器，触发 "Fibers belong to different schedulers"。
  auto* pb = pool_->GetNextProactor();
  pb->Await([&] {
    std::vector<::util::fb2::Fiber> fibers;
    fibers.reserve(hosts.size());
    for (const auto& host : hosts) {
      fibers.push_back(::util::MakeFiber([&, host] {
        auto lat = Probe(host, pb);  // 直接调用（已在 pb 调度器内），不再 pb->Await
        if (lat) {
          std::lock_guard<::util::fb2::Mutex> lk(mu);
          results.push_back({host, *lat});
        }
      }));
    }
    for (auto& f : fibers) f.Join();
  });

  if (results.empty()) return std::nullopt;
  auto best = std::min_element(results.begin(), results.end(),
                               [](const ProbeResult& a, const ProbeResult& b) {
                                 return a.latency_ms < b.latency_ms;
                               });
  return best->host;
}

}  // namespace tdx::proto
