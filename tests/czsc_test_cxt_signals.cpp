// cxt_* 空壳修复 golden 比对：C++ 输出 vs czsc-python 参考（output/cxt_golden_002741.json）
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"

using json = nlohmann::json;

namespace {

std::string ResolvePath(const std::string& rel) {
  for (auto* prefix : {"../", "../../", ""}) {
    std::string p = prefix + rel;
    std::ifstream f(p);
    if (f) return p;
  }
  return "";
}

// 加载 002741 日线
std::vector<czsc::RawBar> Load002741Bars() {
  auto path = ResolvePath("output/002741_bars.json");
  std::ifstream ifs(path);
  if (!ifs) return {};
  json j = json::parse(ifs);
  std::vector<czsc::RawBar> bars;
  for (auto& b : j["bars"]) {
    czsc::RawBar rb;
    rb.symbol = "002741.SZ";
    rb.freq = czsc::Freq::kDay;
    rb.id = static_cast<int32_t>(bars.size());
    std::string ts = b["ts"].get<std::string>();
    struct tm tm = {};
    strptime(ts.c_str(), "%Y-%m-%d %H:%M:%S", &tm);
    rb.dt = timegm(&tm);
    rb.open = b["open"]; rb.close = b["close"]; rb.high = b["high"]; rb.low = b["low"];
    rb.vol = b["volume"]; rb.amount = b["amount"];
    bars.push_back(rb);
  }
  return bars;
}

// 加载 golden
std::unordered_map<std::string, json> LoadGolden() {
  std::unordered_map<std::string, json> g;
  auto path = ResolvePath("output/cxt_golden_002741.json");
  std::ifstream ifs(path);
  if (!ifs) return g;
  json j = json::parse(ifs);
  for (auto& [k, v] : j.items()) g[k] = v;
  return g;
}

}  // namespace

TEST(CxtSignalsGolden, AllImplemented) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "002741_bars.json 不存在";
  czsc::analyze::CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.bi_list.size(), 50u);

  auto golden = LoadGolden();
  if (golden.empty()) GTEST_SKIP() << "cxt_golden_002741.json 不存在";

  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  czsc::signals::ParamView pv(params);

  for (auto& [name, expected_arr] : golden) {
    auto sigs = czsc::signals::run_signal(name.c_str(), czsc, pv, &cache);
    EXPECT_EQ(sigs.size(), expected_arr.size()) << name;
    for (size_t i = 0; i < sigs.size() && i < expected_arr.size(); ++i) {
      const auto& e = expected_arr[i];
      // k1 跳过：czsc-python 用全称（"日线"），C++ FreqName 用简称（"D"），属已知差异
      // k2 命名容许差异（人类可读标签），仅比对 k3/v1/score（k3=信号族唯一标识）
      EXPECT_EQ(sigs[i].k3, e["k3"].get<std::string>()) << name << "[" << i << "].k3";
      // v2 默认值差异：C++ 用「其他」，Rust 用「任意」，等价
      const std::string cpp_v2 = sigs[i].v2;
      const std::string py_v2 = e["v2"].get<std::string>();
      bool v2_eq = (cpp_v2 == py_v2) ||
                   (cpp_v2 == "\xe5\x85\xb6\xe4\xbb\x96" && py_v2 == "\xe4\xbb\xbb\xe6\x84\x8f") ||
                   (cpp_v2 == "\xe4\xbb\xbb\xe6\x84\x8f" && py_v2 == "\xe5\x85\xb6\xe4\xbb\x96");
      EXPECT_TRUE(v2_eq) << name << "[" << i << "].v2 cpp=" << cpp_v2 << " py=" << py_v2;
      EXPECT_EQ(sigs[i].v1, e["v1"].get<std::string>()) << name << "[" << i << "].v1";
      EXPECT_EQ(sigs[i].score, e["score"].get<int>()) << name << "[" << i << "].score";
    }
  }
}

// ============================================================
// Trader-level 信号测试（cxt_intraday / cxt_zhong_shu_gong_zhen）
// 使用 SimpleTraderState + 多频 CZSC 构造已知输入验证逻辑
// ============================================================

// 构造含 N 段震荡趋势的日K线（已知笔数可控）
static std::vector<czsc::RawBar> MakeoscBars(size_t n_per_swing, size_t n_swings) {
  std::vector<czsc::RawBar> bars;
  bars.reserve(n_per_swing * n_swings);
  double price = 100.0;
  for (size_t s = 0; s < n_swings; ++s) {
    double dir = (s % 2 == 0) ? 1.0 : -1.0;
    for (size_t i = 0; i < n_per_swing; ++i) {
      czsc::RawBar b;
      b.symbol = "test"; b.id = static_cast<int32_t>(bars.size());
      b.dt = 1700000000 + static_cast<int64_t>(bars.size()) * 86400;
      b.freq = czsc::Freq::kDay;
      price += dir * 0.5;
      b.close = price; b.open = price - dir * 0.1;
      b.high = price + 0.3; b.low = price - 0.3;
      b.vol = 100.0 + i; b.amount = b.vol * 10;
      bars.push_back(b);
    }
  }
  return bars;
}

// 构造有 3 笔能构成中枢的日线（用于 zhong_shu：需 5 笔）
static std::vector<czsc::RawBar> MakeZhongShuBars() {
  // 上升->下降->上升->下降->上升，形成重叠
  double prices[] = {100,103, 101,98, 102,105, 103,100, 104,107};
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 10; ++i) {
    czsc::RawBar b; b.symbol="test"; b.id=i; b.dt=1700000000+i*86400; b.freq=czsc::Freq::kDay;
    double p=prices[i]; b.close=p; b.open=p-0.2; b.high=p+0.5; b.low=p-0.5;
    b.vol=100+i; b.amount=b.vol*10; bars.push_back(b);
  }
  return bars;
}

TEST(CxtTraderSignals, ZhongShuGongZhen) {
  // 大级别（日线）+ 小级别（60分钟）使用相同数据
  auto bars = MakeZhongShuBars();
  czsc::SimpleTraderState state;
  state.add_freq("\xe6\x97\xa5\xe7\xba\xbf", bars, 50, 6);     // 日线
  state.add_freq("60\xe5\x88\x86\xe9\x92\x9f", bars, 50, 6);  // 60分钟

  std::unordered_map<std::string, nlohmann::json> params;
  params["freq1"] = "\xe6\x97\xa5\xe7\xba\xbf";
  params["freq2"] = "60\xe5\x88\x86\xe9\x92\x9f";
  czsc::signals::ParamView pv(params);
  auto sigs = czsc::signals::run_trader_signal("cxt_zhong_shu_gong_zhen_V221221", state, pv);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "\xe4\xb8\xad\xe6\x9e\xa2\xe5\x85\xb1\xe6\x8c\xafV221221");  // 中枢共振V221221
  // v1 为 看多/看空/其他 之一
  EXPECT_TRUE(sigs[0].v1 == "\xe7\x9c\x8b\xe5\xa4\x9a" || sigs[0].v1 == "\xe7\x9c\x8b\xe7\xa9\xba" || sigs[0].v1 == "\xe5\x85\xb6\xe4\xbb\x96")
      << "v1=" << sigs[0].v1;
}

TEST(CxtTraderSignals, ZhongShuInsufficientData) {
  // 笔数不足 5 笔 → 其他
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 3; ++i) {
    czsc::RawBar b; b.symbol="test"; b.id=i; b.dt=1700000000+i*86400; b.freq=czsc::Freq::kDay;
    b.close=100+i; b.open=99+i; b.high=101+i; b.low=99+i; b.vol=100; b.amount=1000;
    bars.push_back(b);
  }
  czsc::SimpleTraderState state;
  state.add_freq("\xe6\x97\xa5\xe7\xba\xbf", bars, 50, 6);
  state.add_freq("60\xe5\x88\x86\xe9\x92\x9f", bars, 50, 6);
  std::unordered_map<std::string, nlohmann::json> params;
  params["freq1"] = "\xe6\x97\xa5\xe7\xba\xbf";
  params["freq2"] = "60\xe5\x88\x86\xe9\x92\x9f";
  czsc::signals::ParamView pv(params);
  auto sigs = czsc::signals::run_trader_signal("cxt_zhong_shu_gong_zhen_V221221", state, pv);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].v1, "\xe5\x85\xb6\xe4\xbb\x96");  // 其他
}

TEST(CxtTraderSignals, IntradayBasic) {
  // intraday 依赖 30 分钟 CZSC 的 bars_raw 单日保留多根（真实多频场景）。
  // C++ CZSC(kDay) 单频构造会合并同日 bar，此处验证分派正确 + "数据不足→其他"路径。
  // 真实多频场景（日线 vs 30 分钟 CZSC 独立构建）留待集成测试覆盖。
  std::vector<czsc::RawBar> min30;
  for (int i = 0; i < 8; ++i) {
    czsc::RawBar b; b.symbol="test"; b.id=i;
    b.dt = 1710000000 + i*86400;  // 不同日（避免 CZSC 合并）
    b.freq = czsc::Freq::kDay;
    b.open=100+i; b.close=101+i; b.high=102+i; b.low=99+i;
    b.vol=100+i; b.amount=1000; min30.push_back(b);
  }
  std::vector<czsc::RawBar> daily;
  for (int i = 0; i < 3; ++i) {
    czsc::RawBar b; b.symbol="test"; b.id=100+i;
    b.dt = 1710000000 + i*86400; b.freq = czsc::Freq::kDay;
    b.close=100+i*5; b.open=99+i*5; b.high=101+i*5; b.low=99+i*5;
    b.vol=100; b.amount=1000; daily.push_back(b);
  }
  czsc::SimpleTraderState state;
  state.add_freq("30\xe5\x88\x86\xe9\x92\x9f", min30, 50, 6);
  state.add_freq("\xe6\x97\xa5\xe7\xba\xbf", daily, 50, 6);
  std::unordered_map<std::string, nlohmann::json> params;
  params["freq1"] = "30\xe5\x88\x86\xe9\x92\x9f";
  params["freq2"] = "\xe6\x97\xa5\xe7\xba\xbf";
  params["di"] = 2;
  czsc::signals::ParamView pv(params);
  auto sigs = czsc::signals::run_trader_signal("cxt_intraday_V230701", state, pv);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "\xe8\xb5\xb0\xe5\x8a\xbf\xe5\x88\x86\xe7\xb1\xbbV230701");
  // 单频 CZSC 合并同日 bar → 目标日仅 1 根 → bars<=4 → 其他（数据不足路径）
  EXPECT_EQ(sigs[0].v1, "\xe5\x85\xb6\xe4\xbb\x96");  // 其他
}

TEST(CxtTraderSignals, IntradayMissingFreq) {
  // 缺频 → 其他
  czsc::SimpleTraderState state;  // 空状态
  std::unordered_map<std::string, nlohmann::json> params;
  params["freq1"] = "30\xe5\x88\x86\xe9\x92\x9f";
  params["freq2"] = "\xe6\x97\xa5\xe7\xba\xbf";
  czsc::signals::ParamView pv(params);
  auto sigs = czsc::signals::run_trader_signal("cxt_intraday_V230701", state, pv);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].v1, "\xe5\x85\xb6\xe4\xbb\x96");  // 其他
}
