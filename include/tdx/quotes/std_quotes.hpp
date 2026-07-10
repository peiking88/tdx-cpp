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

  // 除权除息事件（msg_id 0x0f）。对齐 opentdx tdxClient.stock_xdxr。
  std::vector<Xdxr> GetXdxr(Market market, std::string_view code);

  // 财务数据 0x10
  Finance GetFinance(Market market, std::string_view code);
  // F10 分类目录 0x2cf
  std::vector<F10Category> GetF10Category(Market market, std::string_view code);
  // F10 内容 0x2d0
  F10Content GetF10Content(Market market, std::string_view code,
                           std::string_view filename, uint32_t start, uint32_t length);
  // F10 单分类全文：分页累积 raw GBK，末尾统一 gbk_to_utf8（避免双字节字符在分页边界切断）。
  std::string GetF10FullText(Market market, std::string_view code, const F10Category& cat);
  // 历史委托 0xfb4
  std::vector<HistoryOrder> GetHistoryOrders(Market market, std::string_view code, uint32_t date_yyyymmdd);
  // 历史逐笔 0xfb5
  std::vector<HistoryTransaction> GetHistoryTransaction(Market market, std::string_view code,
                                                         uint32_t date_yyyymmdd,
                                                         uint16_t start, uint16_t count);
  // 成交量分布 0x51a
  VolProfile GetVolumeProfile(Market market, std::string_view code);
  // 指数信息 0x51d
  IndexInfo GetIndexInfo(Market market, std::string_view code);
  // 主力异动 0x563
  std::vector<UnusualItem> GetUnusual(Market market, uint16_t start = 0, uint16_t count = 600);

  // 默认服务器列表（供 CLI/batch 等复用，避免多处硬编码）
  static std::vector<proto::ServerInfo> DefaultHosts();

 private:
  // 在 fiber 内执行一次请求（受 RetryPolicy + CircuitBreaker 保护）。
  proto::Response Call(uint16_t msg_id, const std::vector<uint8_t>& body);

  // 在 proactor 线程的 fiber 内执行 fn，捕获一切异常转为默认值返回。
  // 原因：helio WorkerFiberImpl::run_ 的 std::apply 不捕获，若 fn 抛出异常
  // 会直通 fiber 入口 → std::terminate() → SIGABRT（rc=-6）。Call() 重试耗尽
  // 时 throw TdxConnectionError，经 proactor_->Await 跑在 worker fiber 里即命中此路径。
  // 异常在此处记 ERROR 后吞掉，返回 R{}（空 vector/零值），外层按 empty() 跳过，语义不变。
  template <typename F>
  auto SafeAwait(F&& fn) -> decltype(fn());
  std::error_code ConnectInFiber();
  void SendHeartbeat();

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
