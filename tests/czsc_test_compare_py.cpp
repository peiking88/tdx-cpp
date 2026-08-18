// czsc-cpp vs czsc-python 回归测试
// 以 czsc-python 输出为 golden，验证 czsc-cpp 分析结果一致
// 数据：光华科技 002741.SZ，2714 根日线 K 线（来源 TDengine）
//
// 若 output/002741_bars.json 缺失则跳过（数据文件未入库）
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"

using json = nlohmann::json;

namespace {

// 从 JSON 加载 002741 日线数据，不存在则返回空
std::vector<czsc::RawBar> Load002741Bars() {
  // 尝试多个路径（ctest 从 build/ 运行，直接运行从 build/tests/ 或项目根）
  const char* candidates[] = {
    "../output/002741_bars.json",    // ctest 从 build/ 运行
    "../../output/002741_bars.json", // 从 build/tests/ 直接运行
    "output/002741_bars.json",       // 从项目根运行
  };
  std::ifstream ifs;
  for (auto* p : candidates) {
    ifs.open(p);
    if (ifs) break;
  }
  if (!ifs) return {};

  json j = json::parse(ifs);
  std::vector<czsc::RawBar> bars;
  for (auto& b : j["bars"]) {
    czsc::RawBar rb;
    rb.symbol = "002741.SZ";
    rb.freq = czsc::Freq::kDay;

    std::string ts = b["ts"].get<std::string>();
    struct tm tm = {};
    strptime(ts.c_str(), "%Y-%m-%d %H:%M:%S", &tm);
    rb.dt = timegm(&tm);

    rb.id = static_cast<int>(bars.size());
    rb.open = b["open"];   rb.close = b["close"];
    rb.high = b["high"];   rb.low = b["low"];
    rb.vol = b["volume"];  rb.amount = b["amount"];
    bars.push_back(rb);
  }
  return bars;
}

}  // namespace

// ============================================================
// 002741 真实数据对比 czsc-python golden
// ============================================================

TEST(CzscComparePython, DataLoad) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "output/002741_bars.json 不存在，跳过对比测试";
  EXPECT_EQ(bars.size(), 2714u);
  EXPECT_DOUBLE_EQ(bars[0].open, 16.25);
  EXPECT_DOUBLE_EQ(bars.back().close, 35.2);
}

TEST(CzscComparePython, FxCount) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "output/002741_bars.json 不存在";

  czsc::analyze::CZSC czsc(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  EXPECT_EQ(fxs.size(), 208u);
}

TEST(CzscComparePython, BiCount) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "output/002741_bars.json 不存在";

  czsc::analyze::CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.bi_list.size(), 50u);
}

TEST(CzscComparePython, BiGoldenValues) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "output/002741_bars.json 不存在";

  czsc::analyze::CZSC czsc(bars, 50, 6);
  ASSERT_EQ(czsc.bi_list.size(), 50u);

  auto& bi = czsc.bi_list;

  // === Golden values from czsc-python (已验证一致) ===

  // BI 0–2: 前 3 笔
  EXPECT_EQ(bi[0].direction, czsc::Direction::kDown);
  EXPECT_NEAR(bi[0].get_high(), 14.9, 1e-4);
  EXPECT_NEAR(bi[0].get_low(), 9.0, 1e-4);
  EXPECT_NEAR(bi[0].get_power_price(), 5.9, 1e-4);
  EXPECT_NEAR(bi[0].get_change(), -0.396, 1e-3);
  EXPECT_EQ(bi[0].get_length(), 25u);

  EXPECT_EQ(bi[1].direction, czsc::Direction::kUp);
  EXPECT_NEAR(bi[1].get_high(), 14.39, 1e-4);
  EXPECT_NEAR(bi[1].get_low(), 9.0, 1e-4);
  EXPECT_NEAR(bi[1].get_power_price(), 5.39, 1e-4);
  EXPECT_NEAR(bi[1].get_change(), 0.5989, 1e-3);
  EXPECT_EQ(bi[1].get_length(), 11u);

  EXPECT_EQ(bi[2].direction, czsc::Direction::kDown);
  EXPECT_NEAR(bi[2].get_high(), 14.39, 1e-4);
  EXPECT_NEAR(bi[2].get_low(), 10.9, 1e-4);
  EXPECT_NEAR(bi[2].get_power_price(), 3.49, 1e-4);
  EXPECT_NEAR(bi[2].get_change(), -0.2425, 1e-3);
  EXPECT_EQ(bi[2].get_length(), 17u);

  // BI 24–26: 中间 3 笔
  EXPECT_EQ(bi[24].direction, czsc::Direction::kDown);
  EXPECT_NEAR(bi[24].get_high(), 21.43, 1e-4);
  EXPECT_NEAR(bi[24].get_low(), 12.47, 1e-4);
  EXPECT_NEAR(bi[24].get_power_price(), 8.96, 1e-4);
  EXPECT_NEAR(bi[24].get_change(), -0.4181, 1e-3);
  EXPECT_EQ(bi[24].get_length(), 24u);

  EXPECT_EQ(bi[25].direction, czsc::Direction::kUp);
  EXPECT_NEAR(bi[25].get_high(), 16.44, 1e-4);
  EXPECT_NEAR(bi[25].get_low(), 12.47, 1e-4);
  EXPECT_NEAR(bi[25].get_power_price(), 3.97, 1e-4);
  EXPECT_NEAR(bi[25].get_change(), 0.3184, 1e-3);
  EXPECT_EQ(bi[25].get_length(), 17u);

  EXPECT_EQ(bi[26].direction, czsc::Direction::kDown);
  EXPECT_NEAR(bi[26].get_high(), 16.44, 1e-4);
  EXPECT_NEAR(bi[26].get_low(), 14.9, 1e-4);
  EXPECT_NEAR(bi[26].get_power_price(), 1.54, 1e-4);
  EXPECT_NEAR(bi[26].get_change(), -0.0937, 1e-3);
  EXPECT_EQ(bi[26].get_length(), 9u);

  // BI 47–49: 最后 3 笔
  EXPECT_EQ(bi[47].direction, czsc::Direction::kUp);
  EXPECT_NEAR(bi[47].get_high(), 31.16, 1e-4);
  EXPECT_NEAR(bi[47].get_low(), 16.41, 1e-4);
  EXPECT_NEAR(bi[47].get_power_price(), 14.75, 1e-4);
  EXPECT_NEAR(bi[47].get_change(), 0.8988, 1e-3);
  EXPECT_EQ(bi[47].get_length(), 33u);

  EXPECT_EQ(bi[48].direction, czsc::Direction::kDown);
  EXPECT_NEAR(bi[48].get_high(), 31.16, 1e-4);
  EXPECT_NEAR(bi[48].get_low(), 25.31, 1e-4);
  EXPECT_NEAR(bi[48].get_power_price(), 5.85, 1e-4);
  EXPECT_NEAR(bi[48].get_change(), -0.1877, 1e-3);
  EXPECT_EQ(bi[48].get_length(), 8u);

  EXPECT_EQ(bi[49].direction, czsc::Direction::kUp);
  EXPECT_NEAR(bi[49].get_high(), 42.15, 1e-4);
  EXPECT_NEAR(bi[49].get_low(), 25.31, 1e-4);
  EXPECT_NEAR(bi[49].get_power_price(), 16.84, 1e-4);
  EXPECT_NEAR(bi[49].get_change(), 0.6653, 1e-3);
  EXPECT_EQ(bi[49].get_length(), 10u);
}

TEST(CzscComparePython, FxGoldenValues) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "output/002741_bars.json 不存在";

  czsc::analyze::CZSC czsc(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 208u);

  // 首个分型：底分型 fx=13.37
  EXPECT_EQ(fxs[0].mark, czsc::Mark::kD);
  EXPECT_NEAR(fxs[0].fx, 13.37, 1e-4);

  // 末个分型：顶分型 fx=39.5
  EXPECT_EQ(fxs[207].mark, czsc::Mark::kG);
  EXPECT_NEAR(fxs[207].fx, 39.5, 1e-4);

  // 中间抽样：FX[100] 底分型 fx=12.47
  EXPECT_EQ(fxs[100].mark, czsc::Mark::kD);
  EXPECT_NEAR(fxs[100].fx, 12.47, 1e-4);
}

// ============================================================
// 全量 FX/BI 字段一致性检查
// ============================================================

TEST(CzscComparePython, AllBisHaveValidFxs) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "output/002741_bars.json 不存在";

  czsc::analyze::CZSC czsc(bars, 50, 6);

  for (auto& bi : czsc.bi_list) {
    // 每笔起止分型必须存在
    EXPECT_TRUE(bi.fx_a.dt > 0);
    EXPECT_TRUE(bi.fx_b.dt > 0);
    EXPECT_NE(bi.fx_a.dt, bi.fx_b.dt);
    // 笔的起止 dt 一致性
    EXPECT_EQ(bi.start_dt(), bi.fx_a.dt);
    EXPECT_EQ(bi.end_dt(), bi.fx_b.dt);
    // 笔至少包含顶/底各一（除了首尾）
    EXPECT_GE(bi.get_length(), 1u);
    // high >= low
    EXPECT_GE(bi.get_high(), bi.get_low());
  }
}
