// SyncState Phase 4 测试：批次断点续传 + 崩溃恢复。
// custom main（MainInitGuard）：SyncState 用 fb2::Mutex，需 helio 初始化。
#include <gtest/gtest.h>

#include "base/init.h"

#include "tdx/data/sync_state.hpp"

using namespace tdx::data;

TEST(SyncState4, BatchCompleteAndResume) {
  SyncState ss("/tmp/tdx_test_sync4.json");
  ss.Clear();
  ss.Load();
  ss.StartBatch("batch1");
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
