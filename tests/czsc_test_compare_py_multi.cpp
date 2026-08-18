// czsc-cpp vs czsc-python 多标的回归测试
// 覆盖 3 类标的：指数(sz399006) / 个股(sh603799) / 基金(sh510300)
// 以 czsc-python 输出为 golden（output/py_golden_3.json），校验 czsc-cpp 分析结果一致。
//
// 生成：tests/test_compare_py_multi.cpp（由 /tmp/gen_cpp_compare.py 据 py_golden_3.json 生成）
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"

using json = nlohmann::json;
using namespace czsc;
using namespace czsc::analyze;

namespace {

std::vector<RawBar> LoadBars(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) return {};
  json j = json::parse(ifs);
  std::vector<RawBar> bars;
  std::string sym = j.value("symbol", "");
  for (size_t i = 0; i < j["bars"].size(); ++i) {
    auto& b = j["bars"][i];
    RawBar rb;
    rb.symbol = sym;
    rb.freq = Freq::kDay;
    rb.id = static_cast<int32_t>(i);
    std::string ts = b["ts"].get<std::string>();
    struct tm tm = {};
    strptime(ts.c_str(), "%Y-%m-%d %H:%M:%S", &tm);
    rb.dt = timegm(&tm);
    rb.open = b["open"];   rb.close = b["close"];
    rb.high = b["high"];   rb.low  = b["low"];
    rb.vol = b["volume"];  rb.amount = b["amount"];
    bars.push_back(rb);
  }
  return bars;
}

std::string PathFor(const std::string& code) {
  static std::unordered_map<std::string, std::string> cache;
  auto it = cache.find(code);
  if (it != cache.end()) return it->second;
  // 候选必须存 std::string：拼接结果若用 .c_str() 取 const char*，临时量在 ';' 处析构即悬垂。
  // 三种前缀覆盖 ctest 工作目录差异（与 test_compare_py.cpp 的 Load002741Bars 一致）。
  std::string cands[] = {
    "../output/" + code + "_bars.json",     // ctest 从 build/ 运行
    "../../output/" + code + "_bars.json",  // 从 build/tests/ 运行
    "output/" + code + "_bars.json",        // 从项目根运行
  };
  for (const auto& p : cands) {
    std::ifstream f(p);
    if (f) { cache[code] = p; return p; }
  }
  return cache[code] = "output/" + code + "_bars.json";  // 兜底：项目根
}

}  // namespace


// ==================== sz399006 (index) ====================
TEST(Cmpsz399006, DataLoad) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP() << "output/sz399006_bars.json 不存在，跳过";
  EXPECT_EQ(bars.size(), 3913u);
}

TEST(Cmpsz399006, FxCount) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.get_fx_list().size(), 235u);
}

TEST(Cmpsz399006, BiCount) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.bi_list.size(), 49u);
}

TEST(Cmpsz399006, FxEndpoints) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 235u);
  EXPECT_EQ(fxs.front().mark, Mark::kG);
  EXPECT_NEAR(fxs.front().fx, 1983.4600, 1e-4);
  EXPECT_EQ(fxs.back().mark, Mark::kG);
  EXPECT_NEAR(fxs.back().fx, 4060.5300, 1e-4);
}

TEST(Cmpsz399006, BiGoldenSamples) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  ASSERT_EQ(czsc.bi_list.size(), 49u);
  {
    auto& bi = czsc.bi_list[0];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 2035.9100, 1e-4);
    EXPECT_NEAR(bi.get_low(), 1840.2600, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 195.6500, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.1063, 5e-3);
    EXPECT_EQ(bi.get_length(), 8u);
  }
  {
    auto& bi = czsc.bi_list[1];
    EXPECT_EQ(bi.direction, Direction::kDown);
    EXPECT_NEAR(bi.get_high(), 2035.9100, 1e-4);
    EXPECT_NEAR(bi.get_low(), 1792.3800, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 243.5300, 1e-3);
    EXPECT_NEAR(bi.get_change(), -0.1196, 5e-3);
    EXPECT_EQ(bi.get_length(), 25u);
  }
  {
    auto& bi = czsc.bi_list[2];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 1897.6700, 1e-4);
    EXPECT_NEAR(bi.get_low(), 1792.3800, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 105.2900, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.0587, 5e-3);
    EXPECT_EQ(bi.get_length(), 8u);
  }
  {
    auto& bi = czsc.bi_list[46];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 4218.3300, 1e-4);
    EXPECT_NEAR(bi.get_low(), 3133.7800, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 1084.5500, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.3461, 5e-3);
    EXPECT_EQ(bi.get_length(), 34u);
  }
  {
    auto& bi = czsc.bi_list[47];
    EXPECT_EQ(bi.direction, Direction::kDown);
    EXPECT_NEAR(bi.get_high(), 4218.3300, 1e-4);
    EXPECT_NEAR(bi.get_low(), 3755.6100, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 462.7200, 1e-3);
    EXPECT_NEAR(bi.get_change(), -0.1097, 5e-3);
    EXPECT_EQ(bi.get_length(), 9u);
  }
  {
    auto& bi = czsc.bi_list[48];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 4380.4100, 1e-4);
    EXPECT_NEAR(bi.get_low(), 3755.6100, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 624.8000, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.1664, 5e-3);
    EXPECT_EQ(bi.get_length(), 11u);
  }
}

// ==================== sh603799 (stock) ====================
TEST(Cmpsh603799, DataLoad) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP() << "output/sh603799_bars.json 不存在，跳过";
  EXPECT_EQ(bars.size(), 2748u);
}

TEST(Cmpsh603799, FxCount) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.get_fx_list().size(), 255u);
}

TEST(Cmpsh603799, BiCount) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.bi_list.size(), 49u);
}

TEST(Cmpsh603799, FxEndpoints) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 255u);
  EXPECT_EQ(fxs.front().mark, Mark::kG);
  EXPECT_NEAR(fxs.front().fx, 37.7700, 1e-4);
  EXPECT_EQ(fxs.back().mark, Mark::kG);
  EXPECT_NEAR(fxs.back().fx, 43.0900, 1e-4);
}

TEST(Cmpsh603799, BiGoldenSamples) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  ASSERT_EQ(czsc.bi_list.size(), 49u);
  {
    auto& bi = czsc.bi_list[0];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 38.1000, 1e-4);
    EXPECT_NEAR(bi.get_low(), 36.5700, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 1.5300, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.0418, 5e-3);
    EXPECT_EQ(bi.get_length(), 6u);
  }
  {
    auto& bi = czsc.bi_list[1];
    EXPECT_EQ(bi.direction, Direction::kDown);
    EXPECT_NEAR(bi.get_high(), 38.1000, 1e-4);
    EXPECT_NEAR(bi.get_low(), 33.5600, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 4.5400, 1e-3);
    EXPECT_NEAR(bi.get_change(), -0.1192, 5e-3);
    EXPECT_EQ(bi.get_length(), 9u);
  }
  {
    auto& bi = czsc.bi_list[2];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 37.1600, 1e-4);
    EXPECT_NEAR(bi.get_low(), 33.5600, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 3.6000, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.1073, 5e-3);
    EXPECT_EQ(bi.get_length(), 7u);
  }
  {
    auto& bi = czsc.bi_list[46];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 70.8500, 1e-4);
    EXPECT_NEAR(bi.get_low(), 61.9000, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 8.9500, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.1446, 5e-3);
    EXPECT_EQ(bi.get_length(), 9u);
  }
  {
    auto& bi = czsc.bi_list[47];
    EXPECT_EQ(bi.direction, Direction::kDown);
    EXPECT_NEAR(bi.get_high(), 70.8500, 1e-4);
    EXPECT_NEAR(bi.get_low(), 45.5600, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 25.2900, 1e-3);
    EXPECT_NEAR(bi.get_change(), -0.3570, 5e-3);
    EXPECT_EQ(bi.get_length(), 24u);
  }
  {
    auto& bi = czsc.bi_list[48];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 55.5000, 1e-4);
    EXPECT_NEAR(bi.get_low(), 45.5600, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 9.9400, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.2182, 5e-3);
    EXPECT_EQ(bi.get_length(), 8u);
  }
}

// ==================== sh510300 (fund) ====================
TEST(Cmpsh510300, DataLoad) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP() << "output/sh510300_bars.json 不存在，跳过";
  EXPECT_EQ(bars.size(), 3433u);
}

TEST(Cmpsh510300, FxCount) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.get_fx_list().size(), 231u);
}

TEST(Cmpsh510300, BiCount) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.bi_list.size(), 49u);
}

TEST(Cmpsh510300, FxEndpoints) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 231u);
  EXPECT_EQ(fxs.front().mark, Mark::kG);
  EXPECT_NEAR(fxs.front().fx, 3.4300, 1e-4);
  EXPECT_EQ(fxs.back().mark, Mark::kG);
  EXPECT_NEAR(fxs.back().fx, 4.9490, 1e-4);
}

TEST(Cmpsh510300, BiGoldenSamples) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  CZSC czsc(bars, 50, 6);
  ASSERT_EQ(czsc.bi_list.size(), 49u);
  {
    auto& bi = czsc.bi_list[0];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 3.5050, 1e-4);
    EXPECT_NEAR(bi.get_low(), 3.3520, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 0.1500, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.0456, 5e-3);
    EXPECT_EQ(bi.get_length(), 8u);
  }
  {
    auto& bi = czsc.bi_list[1];
    EXPECT_EQ(bi.direction, Direction::kDown);
    EXPECT_NEAR(bi.get_high(), 3.5050, 1e-4);
    EXPECT_NEAR(bi.get_low(), 3.1660, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 0.3400, 1e-3);
    EXPECT_NEAR(bi.get_change(), -0.0967, 5e-3);
    EXPECT_EQ(bi.get_length(), 13u);
  }
  {
    auto& bi = czsc.bi_list[2];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 3.3500, 1e-4);
    EXPECT_NEAR(bi.get_low(), 3.1660, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 0.1800, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.0581, 5e-3);
    EXPECT_EQ(bi.get_length(), 9u);
  }
  {
    auto& bi = czsc.bi_list[46];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 4.9930, 1e-4);
    EXPECT_NEAR(bi.get_low(), 4.7890, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 0.2000, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.0426, 5e-3);
    EXPECT_EQ(bi.get_length(), 8u);
  }
  {
    auto& bi = czsc.bi_list[47];
    EXPECT_EQ(bi.direction, Direction::kDown);
    EXPECT_NEAR(bi.get_high(), 4.9930, 1e-4);
    EXPECT_NEAR(bi.get_low(), 4.7050, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 0.2900, 1e-3);
    EXPECT_NEAR(bi.get_change(), -0.0577, 5e-3);
    EXPECT_EQ(bi.get_length(), 10u);
  }
  {
    auto& bi = czsc.bi_list[48];
    EXPECT_EQ(bi.direction, Direction::kUp);
    EXPECT_NEAR(bi.get_high(), 5.0950, 1e-4);
    EXPECT_NEAR(bi.get_low(), 4.7050, 1e-4);
    EXPECT_NEAR(bi.get_power_price(), 0.3900, 1e-3);
    EXPECT_NEAR(bi.get_change(), 0.0829, 5e-3);
    EXPECT_EQ(bi.get_length(), 11u);
  }
}

// ==================== 全标的 FX 抽样一致性 ====================

TEST(Cmpsz399006, FxSample058) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 235u);
  EXPECT_EQ(fxs[58].mark, Mark::kG);
  EXPECT_NEAR(fxs[58].fx, 1735.6000, 1e-4);
}

TEST(Cmpsz399006, FxSample117) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 235u);
  EXPECT_EQ(fxs[117].mark, Mark::kD);
  EXPECT_NEAR(fxs[117].fx, 2171.2700, 1e-4);
}

TEST(Cmpsz399006, FxSample176) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 235u);
  EXPECT_EQ(fxs[176].mark, Mark::kG);
  EXPECT_NEAR(fxs[176].fx, 3240.3500, 1e-4);
}

TEST(Cmpsz399006, FxSample233) {
  auto bars = LoadBars(PathFor("sz399006"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 235u);
  EXPECT_EQ(fxs[233].mark, Mark::kD);
  EXPECT_NEAR(fxs[233].fx, 3812.6600, 1e-4);
}

TEST(Cmpsh603799, FxSample063) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 255u);
  EXPECT_EQ(fxs[63].mark, Mark::kD);
  EXPECT_NEAR(fxs[63].fx, 20.8200, 1e-4);
}

TEST(Cmpsh603799, FxSample127) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 255u);
  EXPECT_EQ(fxs[127].mark, Mark::kD);
  EXPECT_NEAR(fxs[127].fx, 27.3700, 1e-4);
}

TEST(Cmpsh603799, FxSample191) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 255u);
  EXPECT_EQ(fxs[191].mark, Mark::kD);
  EXPECT_NEAR(fxs[191].fx, 56.0100, 1e-4);
}

TEST(Cmpsh603799, FxSample253) {
  auto bars = LoadBars(PathFor("sh603799"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 255u);
  EXPECT_EQ(fxs[253].mark, Mark::kD);
  EXPECT_NEAR(fxs[253].fx, 40.1000, 1e-4);
}

TEST(Cmpsh510300, FxSample057) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 231u);
  EXPECT_EQ(fxs[57].mark, Mark::kD);
  EXPECT_NEAR(fxs[57].fx, 3.2390, 1e-4);
}

TEST(Cmpsh510300, FxSample115) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 231u);
  EXPECT_EQ(fxs[115].mark, Mark::kD);
  EXPECT_NEAR(fxs[115].fx, 3.9650, 1e-4);
}

TEST(Cmpsh510300, FxSample173) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 231u);
  EXPECT_EQ(fxs[173].mark, Mark::kD);
  EXPECT_NEAR(fxs[173].fx, 4.5500, 1e-4);
}

TEST(Cmpsh510300, FxSample229) {
  auto bars = LoadBars(PathFor("sh510300"));
  if (bars.empty()) GTEST_SKIP();
  auto czsc = CZSC(bars, 50, 6);
  auto fxs = czsc.get_fx_list();
  ASSERT_EQ(fxs.size(), 231u);
  EXPECT_EQ(fxs[229].mark, Mark::kD);
  EXPECT_NEAR(fxs[229].fx, 4.7770, 1e-4);
}
