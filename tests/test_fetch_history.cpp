// fetch-history 真网测试：验证 TdxData::FetchHistory（统一 API，本地优先+网络补缺）。
// 仅在通达信服务器可达时执行（不可达时 GTEST_SKIP，不视为失败）。
// custom main（MainInitGuard）：TdxData 内部 StdQuotes 需 helio ProactorPool。
//
// 对齐原 CLI `tdx fetch-history <sh|sz|bj><code> [code...] [period]`：
//   TdxData::FetchHistory(codes, "", "", period) → 本地优先+网络补缺。
#include <gtest/gtest.h>

#include <memory>
#include <system_error>

#include "base/init.h"

#include "tdx/data/tdx_data.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx;

namespace {
class FetchHistoryTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    g_td = std::make_unique<tdx::data::TdxData>();
    if (auto ec = g_td->Connect()) {
      GTEST_SKIP() << "TdxData 连接失败: " << ec.message();
      return;
    }
    if (!g_td->IsConnected()) {
      GTEST_SKIP() << "TdxData 连接后 IsConnected() 应为 true";
      return;
    }
  }
  static void TearDownTestSuite() {
    if (g_td) g_td->Close();
    g_td.reset();
  }
  void SetUp() override {
    if (!g_td || !g_td->IsConnected()) GTEST_SKIP() << "跳过：fetch-history 连接不可用";
  }
  static std::unique_ptr<tdx::data::TdxData> g_td;
};
std::unique_ptr<tdx::data::TdxData> FetchHistoryTest::g_td;
}  // namespace

// 对齐原 `tdx fetch-history sh600000 1d`（日线，本地优先+网络补缺）。
TEST_F(FetchHistoryTest, DailyBars) {
  auto bars = g_td->FetchHistory({"sh600000"}, "", "", "1d");
  EXPECT_FALSE(bars.empty());
  for (const auto& b : bars) {
    EXPECT_GT(b.open, 0.0);
    EXPECT_GT(b.close, 0.0);
    EXPECT_GE(b.high, b.low);
    EXPECT_GE(b.volume, 0.0);
  }
}

// 分钟线（原 CLI 支持 1m/5m）。
TEST_F(FetchHistoryTest, MinuteBars) {
  auto bars = g_td->FetchHistory({"sh600000"}, "", "", "1m");
  for (const auto& b : bars) {
    auto c = tdx::util::epoch_to_cst(b.datetime);
    int mins = c.hour * 60 + c.minute;
    bool am = (mins >= 570 && mins <= 690);   // 9:30-11:30
    bool pm = (mins >= 780 && mins <= 900);   // 13:00-15:00
    EXPECT_TRUE(am || pm) << "分钟 bar 时间 " << c.hour << ":" << c.minute
                          << " 应在交易时段内";
    EXPECT_GT(b.open, 0.0);
  }
}

// 多代码批拉。
TEST_F(FetchHistoryTest, MultiCode) {
  auto bars = g_td->FetchHistory({"sh600000", "sz000001"}, "", "", "1d");
  // 至少返回一部分（不强制每只都命中，因单只可能本地无文件且网络限流）。
  EXPECT_FALSE(bars.empty());
}

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
