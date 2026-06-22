// 扩展行情（端口 7727，期货/港美股/期权）。组合协议层，提供语义化 API。
// 与 StdQuotes 的关键差异：head=1、不发心跳、ex_hosts、登录 msg_id 0x2454（80B hex）。
#pragma once

#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include "util/fibers/pool.h"
#include "util/fibers/proactor_base.h"

#include "tdx/consts.hpp"
#include "tdx/proto/connection.hpp"
#include "tdx/proto/ex_parsers.hpp"
#include "tdx/proto/retry.hpp"
#include "tdx/proto/server_pool.hpp"
#include "tdx/types.hpp"

namespace tdx::quotes {

class ExtQuotes {
 public:
  ExtQuotes();
  ~ExtQuotes();

  ExtQuotes(const ExtQuotes&) = delete;
  ExtQuotes& operator=(const ExtQuotes&) = delete;

  // 启动：ProactorPool + ex_hosts 选服 + 连接 + 登录（0x2454）。不发心跳。
  std::error_code Connect();
  void Close();
  bool IsConnected() const { return connected_; }

  // K 线（0x23ff）
  std::vector<KLine> Bars(ExMarket market, std::string_view code, Period period,
                          uint16_t start = 0, uint16_t count = kKlineMaxCount);
  // 报价（0x248a）
  std::vector<ExQuote> Quotes(const std::vector<std::pair<ExMarket, std::string>>& codes);
  // 商品列表（0x23f5）
  std::vector<proto::ExListItem> Stocks(uint32_t start = 0, uint16_t count = 1600);
  // 类别列表（0x23f4）
  std::vector<proto::ExCategory> CategoryList();
  // 历史成交（0x2412）
  std::vector<Transaction> HistoryTransactions(ExMarket market, std::string_view code, int ymd);

 private:
  proto::Response Call(uint16_t msg_id, const std::vector<uint8_t>& body);
  std::error_code ConnectInFiber();
  static std::vector<proto::ServerInfo> DefaultHosts();

  std::unique_ptr<::util::ProactorPool> pool_;
  ::util::fb2::ProactorBase* proactor_ = nullptr;
  std::unique_ptr<proto::Connection> conn_;
  std::unique_ptr<proto::ServerPool> server_pool_;
  proto::RetryPolicy retry_;
  proto::CircuitBreaker breaker_;
  bool connected_ = false;
};

}  // namespace tdx::quotes
