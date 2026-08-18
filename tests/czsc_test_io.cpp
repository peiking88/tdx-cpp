// Phase 6 IO 层测试：真实 TDengine + mmap（任一不可用则 GTEST_SKIP）。
// CLAUDE.md：真实测试优先、非必要不 mock；用例不简化。
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "czsc/io/data_loader.hpp"

using czsc::Freq;
using czsc::io::LoaderConfig;
using czsc::io::LoadDailyBars;

namespace {
// 判断 TDengine 可达：尝试加载 002515 历史，空即视为不可达（skip）。
std::vector<czsc::RawBar> TryLoad(const std::string& code, czsc::Market mkt) {
  return LoadDailyBars(code, LoaderConfig::FromEnv(), mkt);
}
}  // namespace

// 历史日线加载：行数、周期、升序、字段健全。
TEST(CzscIo, LoadDailyBarsFromTaos) {
  auto bars = TryLoad("510300", czsc::Market::kSh);
  if (bars.empty()) GTEST_SKIP() << "TDengine 不可达或 k_sh510300_1d 无数据，跳过";
  ASSERT_GT(bars.size(), 50u) << "日线行数应 > 50";

  for (const auto& b : bars) {
    EXPECT_EQ(b.freq, Freq::kDay);
    EXPECT_EQ(b.symbol.substr(0, 6), "510300");
    EXPECT_GT(b.dt, 0);
    // 锚点一致性：所有日线 dt 落在 15:00 CST（07:00 UTC）→ dt%86400==25200。
    EXPECT_EQ(b.dt % 86400, 7 * 3600) << "日线时刻应锚定 15:00 CST";
    EXPECT_GT(b.close, 0);
    EXPECT_LE(b.low, b.high);
    EXPECT_LE(b.low, std::max(b.open, b.close));
    EXPECT_GE(b.high, std::min(b.open, b.close));
  }

  // 升序。
  for (size_t i = 1; i < bars.size(); ++i)
    EXPECT_LT(bars[i - 1].dt, bars[i].dt) << "日线应严格升序";
}

// 拼接去重：若 mmap 实时返回当日 bar，结果中同交易日不得重复。
TEST(CzscIo, ConcatNoDupPerTradingDay) {
  auto bars = TryLoad("510300", czsc::Market::kSh);
  if (bars.empty()) GTEST_SKIP() << "TDengine 不可达，跳过";
  std::set<int64_t> days;
  for (const auto& b : bars) days.insert(b.dt / 86400);
  EXPECT_EQ(days.size(), bars.size()) << "存在同交易日重复 bar（去重失败）";
}

// 配置环境变量覆盖：FromEnv 不抛、字段非空。
TEST(CzscIo, ConfigFromEnv) {
  auto cfg = LoaderConfig::FromEnv();
  EXPECT_FALSE(cfg.taos_host.empty());
  EXPECT_FALSE(cfg.taos_db.empty());
  EXPECT_FALSE(cfg.shm_path.empty());
  EXPECT_GT(cfg.taos_port, 0u);
}
