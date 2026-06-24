// StdQuotes 实现：组合协议层，提供 A 股行情语义化 API。
#include "tdx/quotes/std_quotes.hpp"

#include <utility>

#include <boost/asio/ip/address.hpp>

#include "base/logging.h"  // absl LOG（可观测性）

#include "tdx/errors.hpp"
#include "tdx/proto/frame.hpp"
#include "tdx/util/byte_order.hpp"

namespace tdx::quotes {

StdQuotes::StdQuotes() = default;

StdQuotes::~StdQuotes() { Close(); }

std::vector<proto::ServerInfo> StdQuotes::DefaultHosts() {
  // 内置默认标准行情服务器（从 opentdx const.py main_hosts 摘选）。
  // 完整列表见 cfg/servers.json（由 ServerPool 测速选最快）。
  return {
      {"通达信深圳双线主站1", "110.41.147.114", 7709},
      {"通达信深圳双线主站2", "110.41.2.72", 7709},
      {"通达信深圳双线主站3", "110.41.4.4", 7709},
      {"通达信上海双线主站1", "124.70.176.52", 7709},
      {"通达信上海双线主站2", "122.51.120.217", 7709},
      {"通达信上海双线主站3", "123.60.186.45", 7709},
      {"通达信北京双线主站1", "121.36.54.217", 7709},
      {"通达信广州双线主站1", "124.71.85.110", 7709},
      {"通达信武汉电信主站1", "119.97.185.59", 7709},
  };
}

std::error_code StdQuotes::Connect() {
  pool_.reset(::util::fb2::Pool::IOUring(64));
  pool_->Run();
  proactor_ = pool_->GetNextProactor();
  server_pool_ = std::make_unique<proto::ServerPool>(pool_.get());
  return proactor_->Await([&] { return ConnectInFiber(); });
}

std::error_code StdQuotes::ConnectInFiber() {
  auto hosts = DefaultHosts();
  auto best = server_pool_->SelectBest(hosts);
  if (!best) {
    LOG(ERROR) << "StdQuotes: 全部服务器不可达";
    return std::make_error_code(std::errc::connection_refused);
  }
  LOG(INFO) << "StdQuotes: 选服完成 " << best->name << " " << best->ip << ":" << best->port;

  conn_ = std::make_unique<proto::Connection>(proactor_);
  boost::system::error_code addr_ec;
  auto addr = boost::asio::ip::make_address(best->ip, addr_ec);
  if (addr_ec) return std::make_error_code(std::errc::bad_address);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  auto ec = conn_->Connect(ep);
  if (ec) {
    LOG(ERROR) << "StdQuotes: 连接失败 " << best->ip << ": " << ec.message();
    return ec;
  }
  LOG(INFO) << "StdQuotes: 连接成功 " << best->ip << ":" << best->port;

  // 登录（msg_id 0x0d，请求体 <B=1>）
  try {
    auto login = proto::serialize_login();
    auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                   login.data(), login.size());
    conn_->Call(req);
  } catch (const TdxError& e) {
    LOG(ERROR) << "StdQuotes: 登录失败 " << e.what();
    return std::make_error_code(std::errc::permission_denied);
  }
  LOG(INFO) << "StdQuotes: 登录成功，启动心跳";

  // 心跳（15s 间隔，20 次无业务断开）
  heartbeat_ = std::make_unique<proto::Heartbeat>(
      proactor_, [this] { SendHeartbeat(); }, [] {});
  heartbeat_->Start();

  connected_ = true;
  return {};
}

void StdQuotes::SendHeartbeat() {
  auto hb = proto::serialize_heartbeat();
  auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHeartbeat,
                                 hb.data(), hb.size());
  try {
    conn_->Call(req);
  } catch (...) {
    // 心跳失败忽略，由熔断器/连接状态处理
  }
}

void StdQuotes::Close() {
  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }
  if (conn_) {
    conn_->Close();
    conn_.reset();
  }
  connected_ = false;
  if (pool_) {
    pool_->Stop();
    pool_.reset();
  }
  proactor_ = nullptr;
}

proto::Response StdQuotes::Call(uint16_t msg_id, const std::vector<uint8_t>& body) {
  // 受 CircuitBreaker 保护：OPEN 时拒绝；RetryPolicy 退避重试。
  if (!breaker_.AllowRequest()) {
    LOG(WARNING) << "StdQuotes: 熔断器开启，拒绝请求 msg_id=0x" << std::hex << msg_id;
    throw TdxConnectionError("circuit breaker open");
  }
  auto req = proto::pack_request(proto::kHeadNoZip, 0, msg_id, body.data(), body.size());

  proto::Response resp;
  std::error_code ec = retry_.Execute([&]() -> std::error_code {
    try {
      resp = conn_->Call(req);
      return {};
    } catch (const TdxConnectionError&) {
      return std::make_error_code(std::errc::connection_reset);
    } catch (const TdxProtocolError&) {
      return std::make_error_code(std::errc::bad_message);
    }
  });

  if (ec) {
    breaker_.RecordFailure();
    LOG(ERROR) << "StdQuotes: 请求失败（重试耗尽）msg_id=0x" << std::hex << msg_id
               << ": " << ec.message();
    throw TdxConnectionError("call failed after retries: " + ec.message());
  }
  breaker_.RecordSuccess();
  if (heartbeat_) heartbeat_->NotifyActivity();
  return resp;
}

std::vector<KLine> StdQuotes::Bars(Market market, std::string_view code, Period period,
                                   uint16_t start, uint16_t count, Adjust adjust) {
  auto body = proto::serialize_kline(market, code, period, 1, start, count, adjust);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgKline, body);
    return proto::deserialize_kline(resp.body.data(), resp.body.size(), period);
  });
}

std::vector<Quote> StdQuotes::Quotes(const std::vector<proto::QuoteReq>& stocks) {
  auto body = proto::serialize_quotes_detail(stocks);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgQuotesDetail, body);
    return proto::deserialize_quotes_detail(resp.body.data(), resp.body.size());
  });
}

std::vector<Transaction> StdQuotes::Transactions(Market market, std::string_view code,
                                                 uint16_t start, uint16_t count) {
  auto body = proto::serialize_transaction(market, code, start, count);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgTransaction, body);
    return proto::deserialize_transaction(resp.body.data(), resp.body.size());
  });
}

std::vector<Stock> StdQuotes::Stocks(Market market, uint16_t start, uint16_t count) {
  auto body = proto::serialize_list(market, start, count);
  auto m = market;
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgList, body);
    auto result = proto::deserialize_list(resp.body.data(), resp.body.size());
    for (auto& s : result) s.market = static_cast<int>(m);  // 响应不含市场，按请求填
    return result;
  });
}

uint16_t StdQuotes::StockCount(Market market) {
  auto body = proto::serialize_count(market);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgCount, body);
    return proto::deserialize_count(resp.body.data(), resp.body.size());
  });
}

std::vector<Xdxr> StdQuotes::GetXdxr(Market market, std::string_view code) {
  auto body = proto::serialize_xdxr(market, code);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgXdxr, body);
    return proto::deserialize_xdxr(resp.body.data(), resp.body.size());
  });
}

}  // namespace tdx::quotes
