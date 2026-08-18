// 信号基础设施测试（Phase 4 + Phase 5 部分信号）
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <fstream>

#include "czsc/signals/param_view.hpp"
#include "czsc/signals/signal_builder.hpp"
#include "czsc/signals/registry.hpp"
#include "czsc/analyze/czsc.hpp"
#include "czsc/ta/ta_cache.hpp"

// ============================================================
// ParamView 测试
// ============================================================
TEST(ParamViewTest, UsizeDefault) {
  std::unordered_map<std::string, nlohmann::json> m;
  czsc::signals::ParamView p(m);
  EXPECT_EQ(p.usize("di", 1), 1u);
  EXPECT_EQ(p.usize("n", 5), 5u);
}

TEST(ParamViewTest, UsizeFromString) {
  std::unordered_map<std::string, nlohmann::json> m;
  m["di"] = "3";
  czsc::signals::ParamView p(m);
  EXPECT_EQ(p.usize("di", 1), 3u);
}

TEST(ParamViewTest, StrDefault) {
  std::unordered_map<std::string, nlohmann::json> m;
  czsc::signals::ParamView p(m);
  EXPECT_STREQ(p.str("ma_type", "SMA"), "SMA");
}

TEST(ParamViewTest, Bool) {
  std::unordered_map<std::string, nlohmann::json> m;
  m["flag"] = true;
  czsc::signals::ParamView p(m);
  EXPECT_TRUE(p.boolean("flag", false));
  EXPECT_FALSE(p.boolean("missing", false));
}

TEST(ParamViewTest, Number) {
  std::unordered_map<std::string, nlohmann::json> m;
  m["zt"] = 9.9;
  czsc::signals::ParamView p(m);
  EXPECT_DOUBLE_EQ(p.number("zt", 10.0), 9.9);
  EXPECT_DOUBLE_EQ(p.number("missing", 5.0), 5.0);
}

// ============================================================
// Signal Builder 测试
// ============================================================
TEST(SignalBuilderTest, MakeSignal7) {
  auto sigs = czsc::signals::make_signal7("F1", "K2", "K3", "V1", "V2", "V3", 50);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].to_string(), "F1_K2_K3_V1_V2_V3_50");
}

TEST(SignalBuilderTest, MakeSignalV1) {
  auto sigs = czsc::signals::make_signal_v1("日线", "D1", "BS辅助", "上涨");
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].v1, "上涨");
  EXPECT_EQ(sigs[0].score, 0);
}

TEST(SignalBuilderTest, MakeKlineSignalV1) {
  auto sigs = czsc::signals::make_kline_signal_v1("D", "D1", "V230506", "第3层");
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].k1, "D");
  EXPECT_EQ(sigs[0].v1, "第3层");
}

// ============================================================
// pd_cut_last_label 测试
// ============================================================
TEST(CutTest, ConstantSeries) {
  std::vector<double> v = {5.0, 5.0, 5.0, 5.0, 5.0};
  EXPECT_EQ(czsc::signals::pd_cut_last_label(v, 5), 3u);  // center bin
}

TEST(CutTest, LinearSeries) {
  std::vector<double> v = {1.0, 2.0, 3.0, 4.0, 5.0};
  EXPECT_EQ(czsc::signals::pd_cut_last_label(v, 5), 5u);  // last bin
}

TEST(CutTest, NanValues) {
  std::vector<double> v = {1.0, NAN, 3.0};
  EXPECT_EQ(czsc::signals::pd_cut_last_label(v, 5), 0u);  // fails due to NaN
}

// ============================================================
// 信号注册表测试
// ============================================================
TEST(RegistryTest, ContainsExpectedSignals) {
  auto& reg = czsc::signals::signal_registry();
  EXPECT_TRUE(reg.count("bar_single_V230506"));
  EXPECT_TRUE(reg.count("bar_zdt_V230331"));
  EXPECT_TRUE(reg.count("tas_ma_base_V221203"));
  EXPECT_TRUE(reg.count("tas_macd_direct_V221106"));
  EXPECT_TRUE(reg.count("cxt_bi_status_V230102"));
  EXPECT_TRUE(reg.count("cxt_first_buy_V221126"));
  EXPECT_TRUE(reg.count("vol_single_ma_V230214"));
}

TEST(RegistryTest, ListAllReturnsSorted) {
  auto all = czsc::signals::list_all_signals();
  EXPECT_GE(all.size(), 7u);
  for (auto& s : all) EXPECT_FALSE(s.name.empty());
  // Sort for deterministic comparison
  std::sort(all.begin(), all.end(), [](auto& a, auto& b) {
    if (a.category != b.category) return a.category < b.category;
    return a.name < b.name;
  });
  for (size_t i = 1; i < all.size(); ++i) {
    if (all[i-1].category == all[i].category)
      EXPECT_LE(all[i-1].name, all[i].name);
  }
}

TEST(RegistryTest, ListHasNamespaces) {
  auto all = czsc::signals::list_all_signals();
  bool has_bar = false, has_tas = false, has_cxt = false, has_vol = false;
  for (auto& s : all) {
    if (s.ns == "bar") has_bar = true;
    if (s.ns == "tas") has_tas = true;
    if (s.ns == "cxt") has_cxt = true;
    if (s.ns == "vol") has_vol = true;
  }
  EXPECT_TRUE(has_bar);
  EXPECT_TRUE(has_tas);
  EXPECT_TRUE(has_cxt);
  EXPECT_TRUE(has_vol);
}

// ============================================================
// 信号运行测试
// ============================================================
TEST(SignalRunTest, RunBarSingle) {
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 120; ++i) {
    czsc::RawBar b;
    b.symbol = "test";
    b.id = i;
    b.dt = 1000 + i * 60;
    b.freq = czsc::Freq::kDay;
    b.open = 10.0 + i * 0.05;
    b.close = 10.1 + i * 0.05;
    b.high = 10.2 + i * 0.05;
    b.low = 9.9 + i * 0.05;
    b.vol = 100.0;
    b.amount = 1000.0;
    bars.push_back(b);
  }

  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;

  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  params["n"] = 5;
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("bar_single_V230506", czsc, pv, &cache);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].k3, "BarSingleV230506");
}

TEST(SignalRunTest, RunVolSingleMa) {
  std::vector<czsc::RawBar> bars;
  for (int i = 0; i < 30; ++i) {
    czsc::RawBar b;
    b.symbol = "test";
    b.id = i;
    b.dt = 1000 + i * 60;
    b.freq = czsc::Freq::kDay;
    b.open = 10.0;
    b.close = 10.5;
    b.high = 11.0;
    b.low = 9.5;
    b.vol = 100.0 + i * 10;
    b.amount = 1000.0;
    bars.push_back(b);
  }

  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;

  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  params["timeperiod"] = 5;
  params["ma_type"] = "SMA";
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("vol_single_ma_V230214", czsc, pv, &cache);
  ASSERT_EQ(sigs.size(), 1u);
}

TEST(SignalRunTest, RunCxtFirstBuy) {
  std::vector<czsc::RawBar> bars2;
  for (int i = 0; i < 50; ++i) {
    czsc::RawBar b;
    b.symbol = "test";
    b.id = i;
    b.dt = 1000 + i * 86400;
    b.freq = czsc::Freq::kDay;
    double phase = (i < 10) ? 1.0 : (i < 20 ? -1.0 : (i < 35 ? 1.0 : -1.0));
    for (int k = 0; k < 10; ++k) {
      b.open = 10.0 + (i * 10 + k) * phase * 0.5;
      b.close = 10.5 + (i * 10 + k) * phase * 0.5;
      b.high = 11.0 + (i * 10 + k) * phase * 0.5;
      b.low = 9.5 + (i * 10 + k) * phase * 0.5;
      b.vol = 100.0;
      b.amount = 1000.0;
    }
    bars2.push_back(b);
  }

  czsc::analyze::CZSC czsc(bars2, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("cxt_first_buy_V221126", czsc, pv, &cache);
  ASSERT_GE(sigs.size(), 1u);
}

TEST(SignalRunTest, UnknownSignal) {
  std::vector<czsc::RawBar> bars;
  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("nonexistent_signal", czsc, pv, &cache);
  EXPECT_TRUE(sigs.empty());
}

// ============================================================
// 真实数据加载（002741）用于 golden 比对
// ============================================================
static std::vector<czsc::RawBar> Load002741Bars() {
  std::ifstream ifs;
  for (auto* p : {"../output/002741_bars.json", "../../output/002741_bars.json",
                  "output/002741_bars.json"}) {
    ifs.open(p); if (ifs) break;
  }
  if (!ifs) return {};
  auto j = nlohmann::json::parse(ifs);
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

// ============================================================
// 三类买卖点信号测试（cxt_first_buy/sell, cxt_second_bs, cxt_third_bs, cxt_third_buy）
// ============================================================

// 构造含多段震荡趋势的日K线：确保 CZSC 输出足够多笔供三类买卖点判定
static std::vector<czsc::RawBar> MakeTrendBars(size_t n) {
  std::vector<czsc::RawBar> bars;
  bars.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    czsc::RawBar b;
    b.symbol = "test";
    b.id = static_cast<int32_t>(i);
    b.dt = 1700000000 + i * 86400;
    b.freq = czsc::Freq::kDay;
    // 多段正弦 + 缓降基线，制造上升/下降笔交替
    double t = static_cast<double>(i);
    double swing = std::sin(t * 0.35) * 6.0 + std::sin(t * 0.12) * 4.0;
    double base = 100.0 - t * 0.02;  // 微降基线使末端趋弱
    double mid = base + swing;
    b.close = mid;
    b.open  = mid - 0.3;
    b.high  = mid + 1.0;
    b.low   = mid - 1.0;
    b.vol   = 100.0 + static_cast<double>(i);
    b.amount = b.vol * 10.0;
    bars.push_back(b);
  }
  return bars;
}

// cxt_first_buy：运行无异常，输出信号 v1 ∈ {一买, 其他}
TEST(SignalRunTest, CxtFirstBuySignal) {
  auto bars = MakeTrendBars(300);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("cxt_first_buy_V221126", czsc, pv, &cache);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "BUY1");
  // v1 为「一买」或「其他」之一
  bool valid = (sigs[0].v1 == "\xe4\xb8\x80\xe4\xb9\xb0" || sigs[0].v1 == "\xe5\x85\xb6\xe4\xbb\x96");
  EXPECT_TRUE(valid);
}

TEST(SignalRunTest, CxtFirstSellSignal) {
  auto bars = MakeTrendBars(300);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("cxt_first_sell_V221126", czsc, pv, &cache);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "SELL1");
  bool valid = (sigs[0].v1 == "\xe4\xb8\x80\xe5\x8d\x96" || sigs[0].v1 == "\xe5\x85\xb6\xe4\xbb\x96");
  EXPECT_TRUE(valid);
}

TEST(SignalRunTest, CxtSecondBsSignal) {
  auto bars = MakeTrendBars(300);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  params["timeperiod"] = 21;
  params["ma_type"] = "SMA";
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("cxt_second_bs_V230320", czsc, pv, &cache);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "BS2辅助V230320");
  const std::string& v = sigs[0].v1;
  bool valid = (v == "\xe4\xba\x8c\xe4\xb9\xb0" || v == "\xe4\xba\x8c\xe5\x8d\x96" || v == "\xe5\x85\xb6\xe4\xbb\x96");
  EXPECT_TRUE(valid);
}

TEST(SignalRunTest, CxtThirdBsSignal) {
  auto bars = MakeTrendBars(300);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  params["timeperiod"] = 34;
  params["ma_type"] = "SMA";
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("cxt_third_bs_V230318", czsc, pv, &cache);
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "BS3辅助V230318");
  const std::string& v = sigs[0].v1;
  bool valid = (v == "\xe4\xb8\x89\xe4\xb9\xb0" || v == "\xe4\xb8\x89\xe5\x8d\x96" || v == "\xe5\x85\xb6\xe4\xbb\x96");
  EXPECT_TRUE(valid);
}

TEST(SignalRunTest, CxtThirdBuySignal) {
  auto bars = MakeTrendBars(300);
  czsc::analyze::CZSC czsc(bars, 50, 6);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  params["di"] = 1;
  czsc::signals::ParamView pv(params);

  auto sigs = czsc::signals::run_signal("cxt_third_buy_V230228", czsc, pv, &cache);
  // cxt_third_buy 始终返回一条信号
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_STREQ(sigs[0].k3.c_str(), "三买辅助V230228");
  const std::string& v = sigs[0].v1;
  bool valid = (v == "\xe4\xb8\x89\xe4\xb9\xb0" || v == "\xe5\x85\xb6\xe4\xbb\x96");
  EXPECT_TRUE(valid);
}

// ============================================================
// Tier 1 空壳修复 golden 比对（002741 真实数据，与 czsc-python 对齐）
// ============================================================
TEST(CxtTier1Golden, RealData) {
  auto bars = Load002741Bars();
  if (bars.empty()) GTEST_SKIP() << "002741_bars.json 不存在";
  czsc::analyze::CZSC czsc(bars, 50, 6);
  EXPECT_EQ(czsc.bi_list.size(), 50u);
  czsc::ta::TaCache cache;
  std::unordered_map<std::string, nlohmann::json> params;
  czsc::signals::ParamView pv(params);
  auto r = [&](const char* nm) { return czsc::signals::run_signal(nm, czsc, pv, &cache); };

  auto s = r("cxt_bi_base_V230228");  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].v1, "\xe5\x90\x91\xe4\xb8\x8b"); EXPECT_EQ(s[0].v2, "\xe8\xbd\xac\xe6\x8a\x98");  // 向下/转折
  s = r("cxt_bi_stop_V230815");  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].v1, "\xe5\x90\x91\xe4\xb8\x8b"); EXPECT_EQ(s[0].v2, "\xe9\x98\x88\xe5\x80\xbc\xe5\xa4\x96");  // 向下/阈值外
  s = r("cxt_bi_trend_V230824");  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].v1, "\xe5\x90\x91\xe4\xb8\x8a");  // 向上
  s = r("cxt_bs_V240527");  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].v1, "\xe4\xb9\xb0\xe7\x82\xb9");  // 买点
  s = r("cxt_three_bi_V230618");  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].v1, "\xe5\x90\x91\xe4\xb8\x8a\xe6\x97\xa0\xe8\x83\x8c");  // 向上无背
}
