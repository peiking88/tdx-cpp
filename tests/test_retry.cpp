// retry 单元测试：RetryPolicy 退避序列 + CircuitBreaker 状态机。
// 需 ProactorPool fiber 编排（RetryPolicy::Execute 用 ThisFiber::SleepFor，
// CircuitBreaker 用 fb2::Mutex），故用 custom main + pool fixture。
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <system_error>

#include "base/init.h"
#include "util/fibers/fibers.h"
#include "util/fibers/pool.h"

#include "tdx/proto/retry.hpp"

using namespace tdx::proto;

class RetryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pp_.reset(util::fb2::Pool::Epoll());  // Epoll 后端，测试无需 io_uring
    pp_->Run();
  }
  void TearDown() override { pp_->Stop(); }

  template <typename F>
  void RunInFiber(F&& fn) {
    pp_->GetNextProactor()->Await(std::forward<F>(fn));
  }

  std::unique_ptr<util::ProactorPool> pp_;
};

TEST_F(RetryTest, CircuitBreakerOpensAfterThreshold) {
  RunInFiber([&] {
    CircuitBreaker cb(3, std::chrono::seconds(60));
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kClosed);
    cb.RecordFailure();
    cb.RecordFailure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kClosed);
    cb.RecordFailure();  // 第 3 次 → OPEN
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kOpen);
    EXPECT_FALSE(cb.AllowRequest());  // OPEN 拒绝
  });
}

TEST_F(RetryTest, CircuitBreakerHalfOpenAfterRecovery) {
  RunInFiber([&] {
    CircuitBreaker cb(1, std::chrono::seconds(1));  // 1 次失败即 OPEN，1s 恢复
    cb.RecordFailure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kOpen);
    EXPECT_FALSE(cb.AllowRequest());
    util::ThisFiber::SleepFor(std::chrono::milliseconds(1100));
    EXPECT_TRUE(cb.AllowRequest());  // 到恢复时间 → HALF_OPEN 放行
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kHalfOpen);
  });
}

TEST_F(RetryTest, CircuitBreakerHalfOpenSuccessCloses) {
  RunInFiber([&] {
    CircuitBreaker cb(1, std::chrono::seconds(1));
    cb.RecordFailure();
    util::ThisFiber::SleepFor(std::chrono::milliseconds(1100));
    cb.AllowRequest();     // → HALF_OPEN
    cb.RecordSuccess();    // → CLOSED
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kClosed);
  });
}

TEST_F(RetryTest, CircuitBreakerHalfOpenFailureReopens) {
  RunInFiber([&] {
    CircuitBreaker cb(1, std::chrono::seconds(1));
    cb.RecordFailure();
    util::ThisFiber::SleepFor(std::chrono::milliseconds(1100));
    cb.AllowRequest();    // → HALF_OPEN
    cb.RecordFailure();   // → OPEN
    EXPECT_EQ(cb.state(), CircuitBreaker::State::kOpen);
  });
}

// 连续 HALF_OPEN 探测失败应指数拉长恢复窗（防午休长断网重连风暴）：
//   首次熔断后 1×base（1s）放行；探测失败后再等 1×base 仍应被拒（已升至 2×=2s）。
TEST_F(RetryTest, CircuitBreakerBackoffGrowsAfterProbeFailures) {
  RunInFiber([&] {
    CircuitBreaker cb(1, std::chrono::seconds(1));
    cb.RecordFailure();                          // 首次熔断，recovery_failures_=0
    util::ThisFiber::SleepFor(std::chrono::milliseconds(1100));
    EXPECT_TRUE(cb.AllowRequest());              // 1×base 到期 → HALF_OPEN
    cb.RecordFailure();                          // 探测失败 → recovery_failures_=1，2×base
    util::ThisFiber::SleepFor(std::chrono::milliseconds(1100));
    EXPECT_FALSE(cb.AllowRequest());             // 2×base=2s 未到，仍拒绝
    util::ThisFiber::SleepFor(std::chrono::milliseconds(1100));
    EXPECT_TRUE(cb.AllowRequest());              // 累计 2.2s ≥ 2s → HALF_OPEN
  });
}

TEST_F(RetryTest, RetryPolicyStopsOnSuccess) {
  RunInFiber([&] {
    int calls = 0;
    RetryPolicy rp;
    auto ec = rp.Execute([&] {
      ++calls;
      return std::error_code{};
    });
    EXPECT_FALSE(ec);
    EXPECT_EQ(calls, 1);  // 首次成功，不重试
  });
}

TEST_F(RetryTest, RetryPolicyExhaustsOnFailure) {
  RunInFiber([&] {
    int calls = 0;
    RetryPolicy rp({10, 10});  // 短退避
    auto ec = rp.Execute([&] {
      ++calls;
      return std::make_error_code(std::errc::connection_refused);
    });
    EXPECT_TRUE(ec);
    EXPECT_EQ(calls, 3);  // 首次 + 2 次退避
  });
}

TEST_F(RetryTest, RetryPolicyRetriesThenSucceeds) {
  RunInFiber([&] {
    int calls = 0;
    RetryPolicy rp({10, 10});
    auto ec = rp.Execute([&] {
      ++calls;
      return calls >= 2 ? std::error_code{}
                        : std::make_error_code(std::errc::connection_refused);
    });
    EXPECT_FALSE(ec);
    EXPECT_EQ(calls, 2);  // 首次失败，第二次成功
  });
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);  // helio 初始化（MainInitGuard 在全局命名空间）
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
