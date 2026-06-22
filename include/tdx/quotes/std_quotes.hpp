// A 股标准行情（端口 7709）。组合协议层组件，提供语义化 API。对齐 mootdx StdQuotes。
// 在 Proactor 线程/fiber 内执行连接与请求（fiber 纪律）。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "util/fibers/pool.h"
#include "util/fibers/proactor_base.h"

#include "tdx/consts.hpp"
#include "tdx/proto/connection.hpp"
#include "tdx/proto/heartbeat.hpp"
#include "tdx/proto/parsers.hpp"
#include "tdx/proto/retry.hpp"
#include "tdx/proto/server_pool.hpp"
#include "tdx/types.hpp"

namespace tdx::quotes {

class StdQuotes {
 public:
  StdQuotes();
  ~StdQuotes();

  StdQuotes(const StdQuotes&) = delete;
  StdQuotes& operator=(const StdQuotes&) = delete;

  // 启动：ProactorPool + 选服 + 连接 + 登录 + 心跳。返回 error_code（空=成功）。
  std::error_code Connect();
  void Close();
  bool IsConnected() const { return connected_; }

  // K 线（对齐 mootdx get_k_data）。offset 上限 800。
  std::vector<KLine> Bars(Market market, std::string_view code, Period period,
                          uint16_t start = 0, uint16_t count = kKlineMaxCount,
                          Adjust adjust = Adjust::NONE);
  // 五档报价
  std::vector<Quote> Quotes(const std::vector<proto::QuoteReq>& stocks);
  // 逐笔成交（offset 上限 2000）
  std::vector<Transaction> Transactions(Market market, std::string_view code,
                                        uint16_t start = 0, uint16_t count = kTickMaxCount);
  // 股票列表
  std::vector<Stock> Stocks(Market market, uint16_t start = 0, uint16_t count = 1600);
  // 股票数量
  uint16_t StockCount(Market market);

 private:
  // 在 fiber 内执行一次请求（受 RetryPolicy + CircuitBreaker 保护）。
  proto::Response Call(uint16_t msg_id, const std::vector<uint8_t>& body);
  std::error_code ConnectInFiber();
  void SendHeartbeat();
  static std::vector<proto::ServerInfo> DefaultHosts();

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
