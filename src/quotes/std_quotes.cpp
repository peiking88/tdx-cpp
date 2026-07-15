// StdQuotes 实现：组合协议层，提供 A 股行情语义化 API。
#include "tdx/quotes/std_quotes.hpp"

#include <utility>

#include <boost/asio/ip/address.hpp>

#include "base/logging.h"  // absl LOG（可观测性）
#include "util/fibers/fibers.h"  // ::util::MakeFiber（心跳 dispatcher fiber → 普通 fiber 派发重连）

#include "tdx/errors.hpp"
#include "tdx/proto/frame.hpp"
#include "tdx/util/byte_order.hpp"
#include "tdx/util/gbk.hpp"

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

  // 心跳（15s 间隔，20 次无业务断开 → 触发重连）。
  // on_timeout 传重连回调：连续空闲达阈值说明链路已死，主动重连恢复。
  heartbeat_ = std::make_unique<proto::Heartbeat>(
      proactor_, [this] { SendHeartbeat(); },
      [this] { OnHeartbeatTimeout(); });
  heartbeat_->Start();

  connected_ = true;
  return {};
}

void StdQuotes::SendHeartbeat() {
  if (!conn_) return;  // 重连中 conn_ 已置 null，避免空指针
  auto hb = proto::serialize_heartbeat();
  auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHeartbeat,
                                 hb.data(), hb.size());
  try {
    conn_->Call(req);
  } catch (const std::exception& e) {
    // OnTimer 跑在 dispatcher fiber，不可同步重连 → 派发到普通 fiber。
    LOG(WARNING) << "StdQuotes: 心跳发送失败，触发重连: " << e.what();
    ::util::MakeFiber([this] { Reconnect(); }).Detach();
  } catch (...) {
    LOG(WARNING) << "StdQuotes: 心跳发送失败（未知异常），触发重连";
    ::util::MakeFiber([this] { Reconnect(); }).Detach();
  }
}

void StdQuotes::OnHeartbeatTimeout() {
  // OnTimer 已用 MakeFiber 派发到普通 fiber，可安全重连。
  LOG(WARNING) << "StdQuotes: 心跳连续空闲达阈值，触发重连";
  Reconnect();
}

void StdQuotes::Reconnect() {
  if (reconnecting_.exchange(true)) return;  // ponytail: 幂等，防心跳/超时并发风暴
  if (!proactor_) {
    reconnecting_.store(false);
    return;
  }
  // socket/Periodic 操作须所属线程：统一经 proactor_->Await 派发。
  proactor_->Await([this] {
    LOG(INFO) << "StdQuotes: 重连开始（Close → 重新选服/连接/登录/心跳）";
    if (heartbeat_) { heartbeat_->Stop(); heartbeat_.reset(); }
    if (conn_) { conn_->Close(); conn_.reset(); }
    connected_ = false;
    if (auto ec = ConnectInFiber()) {
      LOG(ERROR) << "StdQuotes: 重连失败: " << ec.message();
      reconnecting_.store(false);
      return;
    }
    breaker_.RecordSuccess();  // 重连成功立刻复位，不等 HALF_OPEN 探测
    reconnecting_.store(false);
    LOG(INFO) << "StdQuotes: 重连成功，熔断器已复位";
  });
}

void StdQuotes::Close() {
  // heartbeat Stop(CancelPeriodic) + conn Close(socket 操作) 都要求在 proactor 线程。
  // Close 常从主线程（析构）调用，须把整个 fiber 资源清理派发到 proactor 线程。
  if (proactor_) {
    proactor_->Await([&] {
      if (heartbeat_) { heartbeat_->Stop(); heartbeat_.reset(); }
      if (conn_) { conn_->Close(); conn_.reset(); }
    });
  }
  connected_ = false;
  server_pool_.reset();        // 须在 pool_ 销毁前释放（持有 pool_ 裸指针）
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

template <typename F>
auto StdQuotes::SafeAwait(F&& fn) -> decltype(fn()) {
  using R = decltype(fn());
  return proactor_->Await([&]() -> R {
    try {
      return std::forward<F>(fn)();
    } catch (const std::exception& e) {
      LOG(ERROR) << "StdQuotes: fiber 内请求异常，转空结果返回: " << e.what();
    } catch (...) {
      LOG(ERROR) << "StdQuotes: fiber 内请求未知异常，转空结果返回";
    }
    return R{};
  });
}

std::vector<KLine> StdQuotes::Bars(Market market, std::string_view code, Period period,
                                   uint16_t start, uint16_t count, Adjust adjust) {
  auto body = proto::serialize_kline(market, code, period, 1, start, count, adjust);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgKline, body);
    return proto::deserialize_kline(resp.body.data(), resp.body.size(), period);
  });
}

std::vector<Quote> StdQuotes::Quotes(const std::vector<proto::QuoteReq>& stocks) {
  auto body = proto::serialize_quotes_detail(stocks);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgQuotesDetail, body);
    return proto::deserialize_quotes_detail(resp.body.data(), resp.body.size());
  });
}

std::vector<Transaction> StdQuotes::Transactions(Market market, std::string_view code,
                                                 uint16_t start, uint16_t count) {
  auto body = proto::serialize_transaction(market, code, start, count);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgTransaction, body);
    return proto::deserialize_transaction(resp.body.data(), resp.body.size());
  });
}

std::vector<Stock> StdQuotes::Stocks(Market market, uint16_t start, uint16_t count) {
  auto body = proto::serialize_list(market, start, count);
  auto m = market;
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgList, body);
    auto result = proto::deserialize_list(resp.body.data(), resp.body.size());
    for (auto& s : result) s.market = static_cast<int>(m);  // 响应不含市场，按请求填
    return result;
  });
}

uint16_t StdQuotes::StockCount(Market market) {
  auto body = proto::serialize_count(market);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgCount, body);
    return proto::deserialize_count(resp.body.data(), resp.body.size());
  });
}

std::vector<Xdxr> StdQuotes::GetXdxr(Market market, std::string_view code) {
  auto body = proto::serialize_xdxr(market, code);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgXdxr, body);
    return proto::deserialize_xdxr(resp.body.data(), resp.body.size());
  });
}

Finance StdQuotes::GetFinance(Market market, std::string_view code) {
  auto body = proto::serialize_finance(market, code);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgFinance, body);
    return proto::deserialize_finance(resp.body.data(), resp.body.size());
  });
}

std::vector<F10Category> StdQuotes::GetF10Category(Market market, std::string_view code) {
  auto body = proto::serialize_f10_category(market, code);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgF10Category, body);
    return proto::deserialize_f10_category(resp.body.data(), resp.body.size());
  });
}

F10Content StdQuotes::GetF10Content(Market market, std::string_view code,
                                     std::string_view filename, uint32_t start, uint32_t length) {
  auto body = proto::serialize_f10_content(market, code, filename, start, length);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgF10Content, body);
    return proto::deserialize_f10_content(resp.body.data(), resp.body.size());
  });
}

// 分页拉取 F10 单分类全文：累积 raw GBK 字节，末尾统一 gbk_to_utf8。
// 单分类可达 76KB，单次响应 ≤65535 字节 → 必须分页循环。
std::string StdQuotes::GetF10FullText(Market market, std::string_view code,
                                      const F10Category& cat) {
  std::string gbk_raw;
  gbk_raw.reserve(cat.length);
  constexpr uint32_t kChunk = 32000;
  for (uint32_t offset = 0; offset < cat.length; ) {
    uint32_t want = std::min<uint32_t>(kChunk, cat.length - offset);
    auto body = proto::serialize_f10_content(market, code, cat.filename,
                                             cat.start + offset, want);
    auto resp = SafeAwait([&] { return Call(proto::kMsgF10Content, body); });
    if (resp.body.size() < 12) break;
    uint16_t got = static_cast<uint16_t>(resp.body[10]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(resp.body[11]) << 8);
    if (got == 0) break;
    if (static_cast<std::size_t>(12) + got > resp.body.size()) break;
    gbk_raw.append(reinterpret_cast<const char*>(resp.body.data() + 12), got);
    offset += got;
  }
  return util::gbk_to_utf8(gbk_raw);
}

std::vector<HistoryOrder> StdQuotes::GetHistoryOrders(Market market, std::string_view code,
                                                       uint32_t date_yyyymmdd) {
  auto body = proto::serialize_history_orders(market, code, date_yyyymmdd);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgHistoryOrders, body);
    return proto::deserialize_history_orders(resp.body.data(), resp.body.size());
  });
}

std::vector<HistoryTransaction> StdQuotes::GetHistoryTransaction(
    Market market, std::string_view code, uint32_t date_yyyymmdd,
    uint16_t start, uint16_t count) {
  auto body = proto::serialize_history_transaction(market, code, date_yyyymmdd, start, count);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgHistoryTransaction, body);
    return proto::deserialize_history_transaction(resp.body.data(), resp.body.size());
  });
}

VolProfile StdQuotes::GetVolumeProfile(Market market, std::string_view code) {
  auto body = proto::serialize_volume_profile(market, code);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgVolumeProfile, body);
    return proto::deserialize_volume_profile(resp.body.data(), resp.body.size());
  });
}

IndexInfo StdQuotes::GetIndexInfo(Market market, std::string_view code) {
  auto body = proto::serialize_index_info(market, code);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgIndexInfo, body);
    return proto::deserialize_index_info(resp.body.data(), resp.body.size());
  });
}

std::vector<UnusualItem> StdQuotes::GetUnusual(Market market, uint16_t start, uint16_t count) {
  auto body = proto::serialize_unusual(market, start, count);
  return SafeAwait([&] {
    auto resp = Call(proto::kMsgUnusual, body);
    return proto::deserialize_unusual(resp.body.data(), resp.body.size());
  });
}

}  // namespace tdx::quotes
