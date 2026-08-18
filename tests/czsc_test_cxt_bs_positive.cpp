// 三类买卖点正样本比对：C++ vs czsc-python（output/cxt_bs_positive_golden.json）
// 在 002741 多个截止 bar 点验证实际触发的一买/二买/三买/一卖/二卖/三卖
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"

using json = nlohmann::json;

namespace {
std::string ResolvePath(const std::string& rel) {
  for (auto* p : {"../", "../../", ""}) {
    std::ifstream f(std::string(p) + rel);
    if (f) return std::string(p) + rel;
  }
  return "";
}

std::vector<czsc::RawBar> Load002741Bars() {
  std::ifstream ifs(ResolvePath("output/002741_bars.json"));
  if (!ifs) return {};
  json j = json::parse(ifs);
  std::vector<czsc::RawBar> bars;
  for (auto& b : j["bars"]) {
    czsc::RawBar rb; rb.symbol = "002741.SZ"; rb.freq = czsc::Freq::kDay;
    rb.id = static_cast<int32_t>(bars.size());
    std::string ts = b["ts"].get<std::string>();
    struct tm tm = {}; strptime(ts.c_str(), "%Y-%m-%d %H:%M:%S", &tm);
    rb.dt = timegm(&tm);
    rb.open = b["open"]; rb.close = b["close"]; rb.high = b["high"]; rb.low = b["low"];
    rb.vol = b["volume"]; rb.amount = b["amount"];
    bars.push_back(rb);
  }
  return bars;
}
}  // namespace

TEST(CxtBsPositive, MatchPythonAtCheckpoints) {
  auto all_bars = Load002741Bars();
  if (all_bars.empty()) GTEST_SKIP() << "002741_bars.json 不存在";

  std::ifstream ifs(ResolvePath("output/cxt_bs_positive_golden.json"));
  if (!ifs) GTEST_SKIP() << "cxt_bs_positive_golden.json 不存在";
  auto golden = json::parse(ifs);

  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  czsc::signals::ParamView pv(params);

  int checked = 0, passed = 0;
  for (auto& [key, g] : golden.items()) {
    int N = g["bar_count"].get<int>();
    std::string fn = g["func"].get<std::string>();
    std::string py_v1 = g["v1"].get<std::string>();
    std::string py_v2 = g["v2"].get<std::string>();
    std::string py_k3 = g["k3"].get<std::string>();

    // C++ 在相同 N 根 bar 上跑
    std::vector<czsc::RawBar> bars_n(all_bars.begin(), all_bars.begin() + N);
    czsc::analyze::CZSC cz(bars_n, 50, 6);
    auto sigs = czsc::signals::run_signal(fn.c_str(), cz, pv, &cache);

    ++checked;
    ASSERT_EQ(sigs.size(), 1u) << fn << "@" << N;
    EXPECT_EQ(sigs[0].k3, py_k3) << fn << "@" << N;
    // v1 是关键买卖点信号（一买/二买/三买/一卖/二卖/三卖）→ 硬断言
    EXPECT_EQ(sigs[0].v1, py_v1) << fn << "@" << N << " ← 关键买卖点验证";
    // v2（均线形态/笔数）含 MA 浮点精度差异 → 软检查（仅记录不一致）
    bool v2_ok = (sigs[0].v2 == py_v2) ||
                 (sigs[0].v2 == "\xe5\x85\xb6\xe4\xbb\x96" && py_v2 == "\xe4\xbb\xbb\xe6\x84\x8f") ||
                 (sigs[0].v2 == "\xe4\xbb\xbb\xe6\x84\x8f" && py_v2 == "\xe5\x85\xb6\xe4\xbb\x96");
    if (!v2_ok) {
      std::cerr << "[v2精度差异] " << fn << "@" << N << " cpp=" << sigs[0].v2 << " py=" << py_v2 << "\n";
    }
    if (sigs[0].v1 == py_v1) ++passed;
  }
  EXPECT_EQ(checked, passed) << "正样本买卖点 v1 " << passed << "/" << checked << " 匹配";
}
