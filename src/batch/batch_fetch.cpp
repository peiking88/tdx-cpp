// 并发批量拉取实现。N worker fiber 分片 + 每 worker 独立 Connection。
#include "tdx/batch/batch_fetch.hpp"

#include <algorithm>
#include <memory>

#include <boost/asio/ip/address.hpp>

#include "util/fibers/fibers.h"
#include "util/fibers/pool.h"
#include "util/fibers/synchronization.h"

#include "tdx/proto/connection.hpp"
#include "tdx/proto/frame.hpp"
#include "tdx/proto/parsers.hpp"
#include "tdx/proto/server_pool.hpp"

namespace tdx::batch {
namespace {

std::vector<proto::ServerInfo> DefaultHosts() {
  return {
      {"通达信深圳主站1", "110.41.147.114", 7709},
      {"通达信深圳主站2", "110.41.2.72", 7709},
      {"通达信上海主站1", "124.70.176.52", 7709},
      {"通达信上海主站2", "122.51.120.217", 7709},
      {"通达信武汉主站", "119.97.185.59", 7709},
  };
}

}  // namespace

std::vector<BatchResult> BatchFetchKline(const std::vector<std::string>& codes,
                                         int concurrency, Period period,
                                         uint16_t start, uint16_t count) {
  std::vector<BatchResult> all;
  if (codes.empty() || concurrency < 1) return all;
  int n = std::min(concurrency, static_cast<int>(codes.size()));

  std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
  pool->Run();
  auto* pb = pool->GetNextProactor();

  proto::ServerPool sp(pool.get());
  auto best = sp.SelectBest(DefaultHosts());
  if (!best) {
    pool->Stop();
    return all;
  }
  auto addr = boost::asio::ip::make_address(best->ip);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  ::util::fb2::Mutex mu;
  pb->Await([&] {
    std::vector<::util::fb2::Fiber> workers;
    workers.reserve(n);
    for (int wi = 0; wi < n; ++wi) {
      workers.push_back(::util::MakeFiber([&, wi] {
        proto::Connection conn(pb);
        if (conn.Connect(ep)) return;
        try {
          auto login = proto::serialize_login();
          auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                         login.data(), login.size());
          conn.Call(req);
        } catch (...) {
          return;
        }
        for (size_t i = static_cast<size_t>(wi); i < codes.size(); i += static_cast<size_t>(n)) {
          BatchResult r;
          r.code = codes[i];
          try {
            auto body = proto::serialize_kline(MarketFromCode(codes[i]), codes[i],
                                               period, 1, start, count, Adjust::NONE);
            auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0,
                                   proto::kMsgKline, body.data(), body.size()));
            r.bars = proto::deserialize_kline(resp.body.data(), resp.body.size(), period);
            r.success = true;
          } catch (...) {
            r.success = false;
          }
          {
            std::lock_guard<::util::fb2::Mutex> lk(mu);
            all.push_back(std::move(r));
          }
        }
        conn.Close();
      }));
    }
    for (auto& w : workers) w.Join();
  });

  pool->Stop();
  return all;
}

}  // namespace tdx::batch
