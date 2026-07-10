#include "tdx/proto/heartbeat.hpp"

#include "util/fibers/fibers.h"

namespace tdx::proto {

Heartbeat::Heartbeat(::util::fb2::ProactorBase* proactor,
                     std::function<void()> send_heartbeat,
                     std::function<void()> on_timeout)
    : proactor_(proactor),
      send_heartbeat_(std::move(send_heartbeat)),
      on_timeout_(std::move(on_timeout)) {}

Heartbeat::~Heartbeat() { Stop(); }

void Heartbeat::Start() {
  if (running_) return;
  running_ = true;
  {
    std::lock_guard<::util::fb2::Mutex> lk(mu_);
    last_activity_ = std::chrono::steady_clock::now();
    idle_count_ = 0;
  }
  // AddPeriodic 必须从该 Proactor 自己的线程调用；ms=15000。
  timer_id_ = proactor_->AddPeriodic(
      static_cast<uint32_t>(kHeartbeatIntervalSec * 1000), [this] { OnTimer(); });
  if (!timer_id_) {
    running_ = false;
    return;
  }
}

void Heartbeat::Stop() {
  if (!running_) return;
  running_ = false;
  if (timer_id_) {
    proactor_->CancelPeriodic(timer_id_);
    timer_id_ = 0;
  }
}

void Heartbeat::NotifyActivity() {
  std::lock_guard<::util::fb2::Mutex> lk(mu_);
  last_activity_ = std::chrono::steady_clock::now();
  idle_count_ = 0;  // 对齐 heartbeat.py:26 有通讯重置计数器
}

void Heartbeat::OnTimer() {
  // 对齐 heartbeat.py:28-46：超过 interval 无业务才发心跳，连续达阈值则断开。
  std::function<void()> send_fn;
  std::function<void()> timeout_fn;
  {
    std::lock_guard<::util::fb2::Mutex> lk(mu_);
    auto now = std::chrono::steady_clock::now();
    auto since_sec = std::chrono::duration_cast<std::chrono::seconds>(
                         now - last_activity_).count();
    if (since_sec < kHeartbeatIntervalSec) {
      return;  // 15s 内有业务，不发送心跳（heartbeat.py:44-46）
    }
    ++idle_count_;
    if (idle_count_ >= kHeartbeatMaxIdle) {
      idle_count_ = 0;
      timeout_fn = on_timeout_;  // 连续达阈值，断开（heartbeat.py:36-39）
    } else {
      send_fn = send_heartbeat_;  // 发心跳
    }
  }
  // 在锁外执行回调，避免 send/timeout 阻塞持有锁。
  // OnTimer 跑在 helio periodic dispatcher fiber，不可同步 Suspend（否则触发
  // "Should not preempt dispatcher" abort）——send_fn 内 conn_->Call 会 Suspend，
  // 故包 MakeFiber 切到普通 fiber 执行，惠及 StdQuotes/SPQuotes 等所有心跳用户。
  if (timeout_fn) {
    ::util::MakeFiber([timeout_fn = std::move(timeout_fn)] { timeout_fn(); }).Detach();
  } else if (send_fn) {
    ::util::MakeFiber([send_fn = std::move(send_fn)] {
      try {
        send_fn();
      } catch (...) {
        // 心跳发送失败（连接断）忽略，由 Connection 状态/熔断器处理
      }
    }).Detach();
  }
}

}  // namespace tdx::proto
