// SPQuotes 实现：SP/MAC 高级行情（mac_hosts）。SP 帧头与标准一致（head=0x0c）。
#include "tdx/quotes/sp_quotes.hpp"

#include <utility>

#include <boost/asio/ip/address.hpp>

#include "base/logging.h"

#include "tdx/errors.hpp"
#include "tdx/proto/frame.hpp"
#include "tdx/proto/parsers.hpp"  // 标准登录 serialize_login

namespace tdx::quotes {

SPQuotes::SPQuotes() = default;
SPQuotes::~SPQuotes() { Close(); }

std::vector<proto::ServerInfo> SPQuotes::DefaultMacHosts() {
  // mac_hosts（端口 7709，opentdx const.py:181-187）—— SP/MAC 板块/资金流专用
  return {
      {"行情主站1", "121.36.248.138", 7709},
      {"行情主站2", "123.60.47.136", 7709},
      {"行情主站3", "121.37.207.165", 7709},
  };
}

std::error_code SPQuotes::Connect() {
  pool_.reset(::util::fb2::Pool::IOUring(64));
  pool_->Run();
  proactor_ = pool_->GetNextProactor();
  server_pool_ = std::make_unique<proto::ServerPool>(pool_.get());
  return proactor_->Await([&] { return ConnectInFiber(); });
}

std::error_code SPQuotes::ConnectInFiber() {
  auto hosts = DefaultMacHosts();
  auto best = server_pool_->SelectBest(hosts);
  if (!best) {
    LOG(ERROR) << "SPQuotes: 全部 mac_hosts 不可达";
    return std::make_error_code(std::errc::connection_refused);
  }
  LOG(INFO) << "SPQuotes: 选服 " << best->ip << ":" << best->port;

  conn_ = std::make_unique<proto::Connection>(proactor_);
  boost::system::error_code addr_ec;
  auto addr = boost::asio::ip::make_address(best->ip, addr_ec);
  if (addr_ec) return std::make_error_code(std::errc::bad_address);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  auto ec = conn_->Connect(ep);
  if (ec) return ec;

  // SP 登录复用标准 msg_id 0x0d（head=0x0c）
  try {
    auto login = proto::serialize_login();
    auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                   login.data(), login.size());
    conn_->Call(req);
  } catch (const TdxError& e) {
    LOG(ERROR) << "SPQuotes: 登录失败 " << e.what();
    return std::make_error_code(std::errc::permission_denied);
  }

  // SP 复用标准心跳（15s）
  heartbeat_ = std::make_unique<proto::Heartbeat>(
      proactor_, [this] { SendHeartbeat(); }, [] {});
  heartbeat_->Start();
  LOG(INFO) << "SPQuotes: 登录成功，SP 模式就绪";
  connected_ = true;
  return {};
}

void SPQuotes::SendHeartbeat() {
  auto hb = proto::serialize_heartbeat();
  auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHeartbeat,
                                 hb.data(), hb.size());
  try {
    conn_->Call(req);
  } catch (...) {
  }
}

void SPQuotes::Close() {
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

proto::Response SPQuotes::Call(uint16_t msg_id, const std::vector<uint8_t>& body) {
  // SP 帧头与标准一致（head=0x0c）。受 RetryPolicy + CircuitBreaker 保护。
  if (!breaker_.AllowRequest()) throw TdxConnectionError("circuit breaker open");
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
    throw TdxConnectionError("sp call failed: " + ec.message());
  }
  breaker_.RecordSuccess();
  if (heartbeat_) heartbeat_->NotifyActivity();
  return resp;
}

}  // namespace tdx::quotes
