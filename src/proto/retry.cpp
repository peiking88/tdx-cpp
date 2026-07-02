#include "tdx/proto/retry.hpp"

#include <utility>

namespace tdx::proto {

// ---- RetryPolicy ----

RetryPolicy::RetryPolicy() : RetryPolicy({100, 500, 1000, 2000}) {}  // opentdx [0.1,0.5,1,2]s

RetryPolicy::RetryPolicy(std::vector<int> backoff_ms) : backoff_ms_(std::move(backoff_ms)) {}

// ---- CircuitBreaker ----

CircuitBreaker::CircuitBreaker() : CircuitBreaker(5, std::chrono::seconds(60)) {}

CircuitBreaker::CircuitBreaker(int failure_threshold, std::chrono::seconds recovery_timeout)
    : failure_threshold_(failure_threshold), recovery_timeout_(recovery_timeout) {}

bool CircuitBreaker::AllowRequest() {
  std::lock_guard<util::fb2::Mutex> lk(mu_);
  if (state_ == State::kOpen) {
    auto now = std::chrono::steady_clock::now();
    if (now - open_time_ >= recovery_timeout_) {
      Transition(State::kHalfOpen);  // 到恢复时间，放一个试探请求
      return true;
    }
    return false;  // OPEN 未到恢复时间，拒绝
  }
  return true;  // CLOSED 或 HALF_OPEN 放行
}

void CircuitBreaker::RecordSuccess() {
  std::lock_guard<util::fb2::Mutex> lk(mu_);
  failure_count_ = 0;
  if (state_ != State::kClosed) Transition(State::kClosed);
}

void CircuitBreaker::RecordFailure() {
  std::lock_guard<util::fb2::Mutex> lk(mu_);
  if (failure_count_ < failure_threshold_ * 10) ++failure_count_;  // 防止 kOpen 下无限增长
  if (state_ == State::kHalfOpen) {
    Transition(State::kOpen);  // 试探失败，重新熔断
  } else if (state_ == State::kClosed && failure_count_ >= failure_threshold_) {
    Transition(State::kOpen);  // 连续达阈值，熔断
  }
}

CircuitBreaker::State CircuitBreaker::state() const {
  std::lock_guard<util::fb2::Mutex> lk(mu_);
  return state_;
}

int CircuitBreaker::failure_count() const {
  std::lock_guard<util::fb2::Mutex> lk(mu_);
  return failure_count_;
}

void CircuitBreaker::Transition(State s) {
  state_ = s;
  if (s == State::kOpen) {
    open_time_ = std::chrono::steady_clock::now();
  } else if (s == State::kClosed) {
    failure_count_ = 0;
  }
}

}  // namespace tdx::proto
