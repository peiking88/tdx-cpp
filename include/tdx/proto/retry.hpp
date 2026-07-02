// RetryPolicy + CircuitBreaker（弹性层）。
// 对齐 opentdx baseStockClient.py:78-87（[0.1,0.5,1,2]s ×4 退避）
// + tdxdata/errors.py:90（CLOSED/OPEN/HALF_OPEN 熔断）。
//
// fiber 纪律：退避用 ThisFiber::SleepFor（非 std::this_thread::sleep_for），
// 状态用 util::fb2::Mutex 保护（非 std::mutex）。
#pragma once

#include <chrono>
#include <system_error>
#include <vector>

#include "util/fibers/fibers.h"          // ThisFiber::SleepFor
#include "util/fibers/synchronization.h"  // fb2::Mutex

namespace tdx::proto {

// 指数退避重试。退避序列默认 [100,500,1000,2000] ms（共 4 次重试）。
class RetryPolicy {
 public:
  RetryPolicy();                                  // 默认退避
  explicit RetryPolicy(std::vector<int> backoff_ms);

  // 执行 fn，失败按退避重试，成功即止。返回空 error_code=成功，否则最后一次错误。
  // 退避用 ThisFiber::SleepFor（须在 fiber 内调用）。
  template <typename Fn>
  std::error_code Execute(Fn&& fn) const {
    std::error_code last_ec;
    // 共执行 1 + backoff_ms_.size() 次（首次 + 每次退避后重试）
    for (std::size_t i = 0; i <= backoff_ms_.size(); ++i) {
      last_ec = fn();
      if (!last_ec) return {};  // 成功
      if (i < backoff_ms_.size()) {
        ::util::ThisFiber::SleepFor(std::chrono::milliseconds(backoff_ms_[i]));
      }
    }
    return last_ec;  // 耗尽
  }

  const std::vector<int>& backoff_ms() const { return backoff_ms_; }

 private:
  std::vector<int> backoff_ms_;
};

// 熔断器状态机。
//   CLOSED → 连续 failure_threshold 次失败 → OPEN
//   OPEN → recovery_timeout 后下一次 AllowRequest → HALF_OPEN（放一个试探请求）
//   HALF_OPEN → 成功 → CLOSED；失败 → OPEN
class CircuitBreaker {
 public:
  enum class State { kClosed, kOpen, kHalfOpen };

  CircuitBreaker();  // 默认 5 次失败阈值 / 60s 恢复
  CircuitBreaker(int failure_threshold, std::chrono::seconds recovery_timeout);

  // 是否放行请求。OPEN（且未到恢复时间）时返回 false。
  bool AllowRequest();

  void RecordSuccess();
  void RecordFailure();

  State state() const;
  int failure_count() const;

 private:
  void Transition(State s);

  mutable ::util::fb2::Mutex mu_;
  State state_ = State::kClosed;
  int failure_count_ = 0;
  int failure_threshold_;
  std::chrono::seconds recovery_timeout_;
  std::chrono::steady_clock::time_point open_time_{};  // 进入 OPEN 的时刻
};

}  // namespace tdx::proto
