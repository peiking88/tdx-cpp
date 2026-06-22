// ExtQuotes 实现：扩展行情（7727，head=1，不发心跳）。结构与 StdQuotes 同构。
#include "tdx/quotes/ext_quotes.hpp"

#include <utility>

#include <boost/asio/ip/address.hpp>

#include "base/logging.h"

#include "tdx/errors.hpp"
#include "tdx/proto/frame.hpp"

namespace tdx::quotes {

ExtQuotes::ExtQuotes() = default;
ExtQuotes::~ExtQuotes() { Close(); }

std::vector<proto::ServerInfo> ExtQuotes::DefaultHosts() {
  // ex_hosts（端口 7727）从 opentdx const.py:152-179 摘选
  return {
      {"扩展市场深圳双线1", "112.74.214.43", 7727},
      {"扩展市场深圳双线2", "120.25.218.6", 7727},
      {"扩展市场深圳双线3", "47.107.75.159", 7727},
      {"扩展市场上海双线1", "106.14.95.149", 7727},
      {"扩展市场上海双线2", "47.102.108.214", 7727},
      {"扩展市场上海双线3", "47.103.86.229", 7727},
      {"扩展市场武汉主站1", "139.9.191.175", 7727},
      {"扩展市场广州双线1", "116.205.143.214", 7727},
  };
}

std::error_code ExtQuotes::Connect() {
  pool_.reset(::util::fb2::Pool::IOUring(64));
  pool_->Run();
  proactor_ = pool_->GetNextProactor();
  server_pool_ = std::make_unique<proto::ServerPool>(pool_.get());
  return proactor_->Await([&] { return ConnectInFiber(); });
}

std::error_code ExtQuotes::ConnectInFiber() {
  auto hosts = DefaultHosts();
  auto best = server_pool_->SelectBest(hosts);
  if (!best) {
    LOG(ERROR) << "ExtQuotes: 全部扩展服务器不可达";
    return std::make_error_code(std::errc::connection_refused);
  }
  LOG(INFO) << "ExtQuotes: 选服 " << best->ip << ":" << best->port;

  conn_ = std::make_unique<proto::Connection>(proactor_);
  boost::system::error_code addr_ec;
  auto addr = boost::asio::ip::make_address(best->ip, addr_ec);
  if (addr_ec) return std::make_error_code(std::errc::bad_address);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  auto ec = conn_->Connect(ep);
  if (ec) return ec;

  // 登录 0x2454（80B hex，head=1）。扩展行情不发心跳。
  try {
    auto login = proto::serialize_ex_login();
    auto req = proto::pack_request(proto::kExHead, 0, proto::kMsgExLogin,
                                   login.data(), login.size());
    conn_->Call(req);
  } catch (const TdxError& e) {
    LOG(ERROR) << "ExtQuotes: 登录失败 " << e.what();
    return std::make_error_code(std::errc::permission_denied);
  }
  LOG(INFO) << "ExtQuotes: 登录成功（不发心跳）";
  connected_ = true;
  return {};
}

void ExtQuotes::Close() {
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

proto::Response ExtQuotes::Call(uint16_t msg_id, const std::vector<uint8_t>& body) {
  // head=1（扩展行情）。受 RetryPolicy + CircuitBreaker 保护。
  if (!breaker_.AllowRequest()) throw TdxConnectionError("circuit breaker open");
  auto req = proto::pack_request(proto::kExHead, 0, msg_id, body.data(), body.size());
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
    throw TdxConnectionError("ex call failed: " + ec.message());
  }
  breaker_.RecordSuccess();
  return resp;
}

std::vector<KLine> ExtQuotes::Bars(ExMarket market, std::string_view code, Period period,
                                   uint16_t start, uint16_t count) {
  auto body = proto::serialize_ex_kline(market, code, period, 1, start, count);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgExKline, body);
    return proto::deserialize_ex_kline(resp.body.data(), resp.body.size(), period);
  });
}

std::vector<ExQuote> ExtQuotes::Quotes(const std::vector<std::pair<ExMarket, std::string>>& codes) {
  auto body = proto::serialize_ex_quotes(codes);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgExQuotes, body);
    return proto::deserialize_ex_quotes(resp.body.data(), resp.body.size());
  });
}

std::vector<proto::ExListItem> ExtQuotes::Stocks(uint32_t start, uint16_t count) {
  auto body = proto::serialize_ex_list(start, count);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgExList, body);
    return proto::deserialize_ex_list(resp.body.data(), resp.body.size());
  });
}

std::vector<proto::ExCategory> ExtQuotes::CategoryList() {
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgExCategoryList, {});
    return proto::deserialize_ex_category_list(resp.body.data(), resp.body.size());
  });
}

std::vector<Transaction> ExtQuotes::HistoryTransactions(ExMarket market, std::string_view code, int ymd) {
  auto body = proto::serialize_ex_history_txn(market, code, ymd);
  return proactor_->Await([&] {
    auto resp = Call(proto::kMsgExHistoryTxn, body);
    return proto::deserialize_ex_history_txn(resp.body.data(), resp.body.size());
  });
}

}  // namespace tdx::quotes
