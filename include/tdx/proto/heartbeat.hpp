// 心跳管理。对齐 opentdx utils/heartbeat.py：
//   DEFAULT_HEARTBEAT_INTERVAL=15s，连续 MAX_CONSECUTIVE_HEARTBEAT=20 次纯心跳无业务则断开。
// 用 helio ProactorBase::AddPeriodic 周期触发（fiber 纪律：fb2::Mutex 保护计数）。
#pragma once

#include <chrono>
#include <functional>

#include "util/fibers/proactor_base.h"
#include "util/fibers/synchronization.h"

#include "tdx/consts.hpp"

namespace tdx::proto {

class Heartbeat {
 public:
  // send_heartbeat：发送一次心跳（由 StdQuotes 提供实际发送逻辑）。
  // on_timeout：连续空闲达阈值时回调（通常触发重连/断开）。
  Heartbeat(::util::fb2::ProactorBase* proactor,
            std::function<void()> send_heartbeat,
            std::function<void()> on_timeout);
  ~Heartbeat();

  Heartbeat(const Heartbeat&) = delete;
  Heartbeat& operator=(const Heartbeat&) = delete;

  // 启动 15s 周期心跳。
  void Start();
  // 停止周期心跳。
  void Stop();
  // 业务请求时调用，重置空闲计数与计时（对齐 update_last_ack_time）。
  void NotifyActivity();

 private:
  void OnTimer();

  ::util::fb2::ProactorBase* proactor_;
  std::function<void()> send_heartbeat_;
  std::function<void()> on_timeout_;
  uint32_t timer_id_ = 0;
  bool running_ = false;

  ::util::fb2::Mutex mu_;  // 保护下面字段
  int idle_count_ = 0;
  std::chrono::steady_clock::time_point last_activity_{};
};

}  // namespace tdx::proto
