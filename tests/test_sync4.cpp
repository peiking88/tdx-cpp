// SyncState Phase 4 测试：批次断点续传 + 崩溃恢复。
// custom main（MainInitGuard）：SyncState 用 fb2::Mutex，需 helio 初始化。
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>

#include "base/init.h"
#include "util/fibers/fibers.h"
#include "util/fibers/pool.h"

#include "tdx/data/sync_state.hpp"

using namespace tdx::data;

TEST(SyncState4, BatchCompleteAndResume) {
  SyncState ss("/tmp/tdx_test_sync4.json");
  ss.Clear();
  ss.Load();
  ss.StartBatch("batch1");
  EXPECT_EQ(ss.GetActiveBatch(), "batch1");  // StartBatch 记录批次 ID
  // 600000 完成，000001 未完成
  ss.MarkStockComplete("600000", "history_kline", "batch1");
  EXPECT_TRUE(ss.IsCompletedInBatch("600000", "history_kline", "batch1"));
  EXPECT_FALSE(ss.IsCompletedInBatch("000001", "history_kline", "batch1"));

  // 模拟崩溃恢复：新实例 Load 同一文件
  SyncState ss2("/tmp/tdx_test_sync4.json");
  ss2.Load();
  EXPECT_TRUE(ss2.IsCompletedInBatch("600000", "history_kline", "batch1"));
  EXPECT_FALSE(ss2.IsCompletedInBatch("000001", "history_kline", "batch1"));
  ss2.Clear();
}

TEST(SyncState4, MarkFailedThenComplete) {
  SyncState ss("/tmp/tdx_test_sync4b.json");
  ss.Clear();
  ss.MarkStockFailed("000001", "history_kline", "batch1", "timeout");
  // 失败后 IsCompletedInBatch 应 false
  EXPECT_FALSE(ss.IsCompletedInBatch("000001", "history_kline", "batch1"));
  // 成功覆盖
  ss.MarkStockComplete("000001", "history_kline", "batch1");
  EXPECT_TRUE(ss.IsCompletedInBatch("000001", "history_kline", "batch1"));
  ss.Clear();
}

TEST(SyncState4, FiberConcurrentWrite) {
  // 2 fibers 同时写不同 stock → 验证无 data race / 无状态丢失
  auto pp = std::unique_ptr<util::ProactorPool>(util::fb2::Pool::Epoll());
  pp->Run();

  std::string path = "/tmp/tdx_test_sync4_concurrent.json";
  SyncState ss(path);
  ss.Clear();
  ss.StartBatch("batch_concurrent");

  pp->GetNextProactor()->Await([&] {
    util::fb2::Done done_a, done_b;
    // Fiber A: 写 600000-600004
    util::fb2::Fiber("write_a", [&ss, &done_a] {
      for (int i = 0; i < 5; ++i) {
        ss.MarkStockComplete("60000" + std::to_string(i), "history_kline", "batch_concurrent");
      }
      done_a.Notify();
    }).Detach();
    // Fiber B: 写 000005-000009
    util::fb2::Fiber("write_b", [&ss, &done_b] {
      for (int i = 0; i < 5; ++i) {
        ss.MarkStockFailed("00000" + std::to_string(5 + i), "history_kline",
                           "batch_concurrent", "err");
      }
      done_b.Notify();
    }).Detach();

    done_a.WaitFor(std::chrono::seconds(5));
    done_b.WaitFor(std::chrono::seconds(5));
  });

  // 验证：新实例 Load → 全部 10 只股票状态存在
  SyncState ss2(path);
  ss2.Load();
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(ss2.IsCompletedInBatch("60000" + std::to_string(i), "history_kline",
                                       "batch_concurrent"));
    EXPECT_FALSE(ss2.IsCompletedInBatch("00000" + std::to_string(5 + i), "history_kline",
                                        "batch_concurrent"));
  }
  ss2.Clear();
}

TEST(SyncState4, CorruptedJson) {
  // 写入损坏 JSON → Load 应静默降级（不崩溃，状态为空）
  {
    std::ofstream f("/tmp/tdx_test_sync4_corrupt.json");
    f << "this is not valid json {{{[[[";
  }
  SyncState ss("/tmp/tdx_test_sync4_corrupt.json");
  ss.Load();
  EXPECT_EQ(ss.GetLastSync("600000", "history_kline"), "");
  ss.Clear();
}

TEST(SyncState4, LastSyncBackwardCompat) {
  // 旧字段 last_sync 仍可用（向后兼容）
  SyncState ss("/tmp/tdx_test_sync4c.json");
  ss.Clear();
  ss.UpdateSync("600000", "history_kline", "2024-06-22");
  EXPECT_EQ(ss.GetLastSync("600000", "history_kline"), "2024-06-22");
  ss.Clear();
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);  // helio 初始化（fb2::Mutex 需）
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
