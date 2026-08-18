// 市场类型 + NormalizeCode + 涨跌停市场门控 单元测试。
#include <gtest/gtest.h>

#include "czsc/analyze/czsc.hpp"
#include "czsc/io/signal_store.hpp"
#include "czsc/signals/param_view.hpp"
#include "czsc/signals/registry.hpp"
#include "czsc/types/market.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace {
namespace cz = czsc;

// ---- Market 枚举与互转 ----
TEST(MarketTest, RoundTrip) {
  EXPECT_STREQ(cz::MarketToPrefix(cz::Market::kSh), "sh");
  EXPECT_STREQ(cz::MarketToPrefix(cz::Market::kSz), "sz");
  EXPECT_STREQ(cz::MarketToPrefix(cz::Market::kBj), "bj");
  EXPECT_STREQ(cz::MarketToPrefix(cz::Market::kHk), "hk");

  EXPECT_EQ(cz::PrefixToMarket("sh"), cz::Market::kSh);
  EXPECT_EQ(cz::PrefixToMarket("SZ"), cz::Market::kSz);
  EXPECT_EQ(cz::PrefixToMarket("Bj"), cz::Market::kBj);
  EXPECT_EQ(cz::PrefixToMarket("hk"), cz::Market::kHk);
  EXPECT_EQ(cz::PrefixToMarket("HK"), cz::Market::kHk);
  // 非法前缀回退 kSh
  EXPECT_EQ(cz::PrefixToMarket("xy"), cz::Market::kSh);
}

// ---- io::NormalizeCode ----
TEST(NormalizeCodeTest, BareCodeRejected) {
  // 裸 6 位码不再推断市场，一律拒绝（A 股/港股首字符重叠，无法区分）
  EXPECT_FALSE(czsc::io::NormalizeCode("600519").has_value());
  EXPECT_FALSE(czsc::io::NormalizeCode("002741").has_value());
  EXPECT_FALSE(czsc::io::NormalizeCode("510300").has_value());
}

TEST(NormalizeCodeTest, ExplicitPrefix) {
  auto k = czsc::io::NormalizeCode("sh600519");
  ASSERT_TRUE(k.has_value());
  EXPECT_EQ(k->code, "600519");
  EXPECT_EQ(k->mkt, "sh");
  EXPECT_EQ(k->display, "sh600519");

  auto sz = czsc::io::NormalizeCode("SZ002741");
  ASSERT_TRUE(sz.has_value());
  EXPECT_EQ(sz->mkt, "sz");

  auto bj = czsc::io::NormalizeCode("bj830799");
  ASSERT_TRUE(bj.has_value());
  EXPECT_EQ(bj->mkt, "bj");
}

TEST(NormalizeCodeTest, HkMustHavePrefix) {
  // hk + 5 位码（港股常见）
  auto k = czsc::io::NormalizeCode("hk00700");
  ASSERT_TRUE(k.has_value());
  EXPECT_EQ(k->code, "00700");
  EXPECT_EQ(k->mkt, "hk");
  EXPECT_EQ(k->display, "hk00700");

  // HK 大写前缀
  auto k2 = czsc::io::NormalizeCode("HK03690");
  ASSERT_TRUE(k2.has_value());
  EXPECT_EQ(k2->mkt, "hk");

  // 5 位裸码视为非法（无法区分 A 股/港股）
  EXPECT_FALSE(czsc::io::NormalizeCode("00700").has_value());
  // 6 位裸码同样非法（不再推断市场）
  EXPECT_FALSE(czsc::io::NormalizeCode("000700").has_value());
}

TEST(NormalizeCodeTest, Invalid) {
  EXPECT_FALSE(czsc::io::NormalizeCode("").has_value());
  EXPECT_FALSE(czsc::io::NormalizeCode("12345").has_value());   // 5 位裸码
  EXPECT_FALSE(czsc::io::NormalizeCode("123456").has_value());  // 6 位裸码
  EXPECT_FALSE(czsc::io::NormalizeCode("1234567").has_value()); // 7 位裸码（无前缀）
  EXPECT_FALSE(czsc::io::NormalizeCode("xy123456").has_value());// 非法前缀
  // 带合法前缀时允许 5-6 位数字（HK 5 位场景）：sh+5 位 → 合法。
  EXPECT_TRUE(czsc::io::NormalizeCode("sh60051").has_value());
}

// ---- 涨跌停信号 hk 门控 ----
static std::vector<czsc::RawBar> MakeBars(size_t n) {
  std::vector<czsc::RawBar> bars;
  for (size_t i = 0; i < n; ++i) {
    czsc::RawBar b;
    b.id = static_cast<int>(i);
    b.dt = 1000 + static_cast<int64_t>(i) * 86400;
    b.freq = czsc::Freq::kDay;
    b.open = 10.0;
    b.close = 10.5;
    b.high = 11.0;
    b.low = 9.5;
    b.vol = 100.0;
    b.amount = 1000.0;
    bars.push_back(b);
  }
  return bars;
}

// ParamView 持有指向 inner map 的指针，map 须比 ParamView 活得更久。
struct ParamFixture {
  std::unordered_map<std::string, nlohmann::json> p;
  czsc::signals::ParamView pv;
  ParamFixture(const std::string& market) : p(), pv(p) {
    p["di"] = 1;
    p["market"] = market;
  }
};

TEST(LimitSignalGate, HkReturnsEmpty) {
  auto bars = MakeBars(20);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  ParamFixture f("hk");
  EXPECT_TRUE(czsc::signals::run_signal("bar_limit_down_V230525", czsc, f.pv, nullptr).empty());
  EXPECT_TRUE(czsc::signals::run_signal("bar_zdt_V230331", czsc, f.pv, nullptr).empty());
  EXPECT_TRUE(czsc::signals::run_signal("bar_zt_count_V230504", czsc, f.pv, nullptr).empty());
}

TEST(LimitSignalGate, AshareStillFires) {
  auto bars = MakeBars(20);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  ParamFixture f("sh");
  EXPECT_FALSE(czsc::signals::run_signal("bar_limit_down_V230525", czsc, f.pv, nullptr).empty());
  EXPECT_FALSE(czsc::signals::run_signal("bar_zdt_V230331", czsc, f.pv, nullptr).empty());
  EXPECT_FALSE(czsc::signals::run_signal("bar_zt_count_V230504", czsc, f.pv, nullptr).empty());
}

// ---- 港股数据源：不入 TDengine，LoadDailyBars(kHk) short-circuit ----
// short-circuit 在 DB 连接前返回；shm 路径不存在 → LoadTodayFromShm 返回 nullopt，故恒空。
TEST(HkGate, LoadDailyBarsHkSkipsDb) {
  czsc::io::LoaderConfig cfg;
  cfg.shm_path = "/dev/shm/czsc_test_nonexistent.shm";  // 确保 mmap 不可读
  auto bars = czsc::io::LoadDailyBars("03690", cfg, czsc::Market::kHk);
  EXPECT_TRUE(bars.empty());
}

}  // namespace
