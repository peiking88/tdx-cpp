// 扩展行情（端口 7727，期货/港美股/期权）。组合协议层，提供语义化 API。
// 与 StdQuotes 的关键差异：head=1、不发心跳、ex_hosts、登录 msg_id 0x2454（80B hex）。
#pragma once

#include <atomic>
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

  // 默认扩展行情服务器列表（供 CLI/fetch-quotes 等复用，避免多处硬编码）
  static std::vector<proto::ServerInfo> DefaultHosts();
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
  // 请求失败驱动重连（ex 协议不发心跳，靠 Call 检测到连接断时触发，幂等）。
  void Reconnect();
  // 在 proactor 线程 fiber 内执行 fn，捕获一切异常转空结果返回。
  // 同 StdQuotes::SafeAwait（v0.16.0）：Call 重试耗尽 throw TdxConnectionError，
  // 经 proactor_->Await 跑在 WorkerFiber 里——helio WorkerFiberImpl::run_ 的 std::apply
  // 不捕获异常，会直通 fiber 入口 → std::terminate/SIGABRT。此处记 ERROR 后吞掉，
  // 返回 R{}（空 vector），外层按 empty() 跳过，语义不变。
  template <typename F>
  auto SafeAwait(F&& fn) -> decltype(fn());
  std::error_code ConnectInFiber();

  std::unique_ptr<::util::ProactorPool> pool_;
  ::util::fb2::ProactorBase* proactor_ = nullptr;
  // shared_ptr：Reconnect 换 conn_ 时，在途请求快照保活旧 Connection（防 UAF）
  std::shared_ptr<proto::Connection> conn_;
  std::unique_ptr<proto::ServerPool> server_pool_;
  proto::RetryPolicy retry_;
  proto::CircuitBreaker breaker_;
  bool connected_ = false;
  // 重连幂等标志（atomic：仅布尔，任意 fiber 原子读写）
  std::atomic<bool> reconnecting_{false};
};

}  // namespace tdx::quotes
