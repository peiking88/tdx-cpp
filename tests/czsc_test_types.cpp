// 类型单元测试
// 覆盖: RawBar, NewBar, FX, BI, ZS, Signal 构造/方法/JSON 序列化
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"

// ============================================================
// RawBar 测试（对齐 bar.rs:574-642）
// ============================================================
TEST(RawBarTest, DefaultConstruction) {
  czsc::RawBar bar;
  EXPECT_EQ(bar.freq, czsc::Freq::kDay);
  EXPECT_EQ(bar.id, 0);
  EXPECT_EQ(bar.open, 0.0);
  EXPECT_EQ(bar.close, 0.0);
}

TEST(RawBarTest, ShadowUpBar) {
  // 阴线：open=10, close=12, high=15, low=8
  czsc::RawBar bar;
  bar.open = 10.0;
  bar.close = 12.0;
  bar.high = 15.0;
  bar.low = 8.0;
  EXPECT_DOUBLE_EQ(bar.upper(), 3.0);   // 15 - max(10,12) = 3
  EXPECT_DOUBLE_EQ(bar.lower(), 2.0);   // min(10,12) - 8 = 2
  EXPECT_DOUBLE_EQ(bar.solid(), 2.0);   // |10-12| = 2
}

TEST(RawBarTest, ShadowDownBar) {
  // 阴线：open=12, close=10, high=15, low=8
  czsc::RawBar bar;
  bar.open = 12.0;
  bar.close = 10.0;
  bar.high = 15.0;
  bar.low = 8.0;
  EXPECT_DOUBLE_EQ(bar.upper(), 3.0);   // 15 - max(12,10) = 3
  EXPECT_DOUBLE_EQ(bar.lower(), 2.0);   // min(12,10) - 8 = 2
  EXPECT_DOUBLE_EQ(bar.solid(), 2.0);   // |12-10| = 2
}

TEST(RawBarTest, Doji) {
  czsc::RawBar bar;
  bar.open = 10.0;
  bar.close = 10.0;
  bar.high = 12.0;
  bar.low = 9.0;
  EXPECT_DOUBLE_EQ(bar.solid(), 0.0);
  EXPECT_DOUBLE_EQ(bar.upper(), 2.0);
  EXPECT_DOUBLE_EQ(bar.lower(), 1.0);
}

TEST(RawBarTest, Equality) {
  czsc::RawBar a, b;
  a.symbol = "test";
  a.dt = 1000;
  a.id = 1;
  a.open = 10.0;
  a.close = 12.0;
  a.high = 15.0;
  a.low = 8.0;
  a.vol = 100.0;
  a.amount = 1000.0;

  b = a;
  EXPECT_TRUE(a == b);

  b.id = 2;
  EXPECT_TRUE(a != b);
}

TEST(RawBarTest, JsonRoundTrip) {
  czsc::RawBar bar;
  bar.symbol = "000001.SZ";
  bar.dt = 1705312800;   // 2024-01-15 epoch
  bar.freq = czsc::Freq::kDay;
  bar.id = 42;
  bar.open = 10.5;
  bar.close = 11.2;
  bar.high = 11.5;
  bar.low = 10.3;
  bar.vol = 100000.0;
  bar.amount = 1050000.0;

  nlohmann::json j = bar;
  auto bar2 = j.get<czsc::RawBar>();

  EXPECT_EQ(bar, bar2);
  EXPECT_EQ(j["symbol"], "000001.SZ");
  EXPECT_EQ(j["dt"], 1705312800);
  EXPECT_EQ(j["freq"], "D");
}

// ============================================================
// NewBar 测试（对齐 bar.rs:660-693）
// ============================================================
TEST(NewBarTest, FromRaw) {
  czsc::RawBar raw;
  raw.symbol = "600000.SH";
  raw.dt = 1000;
  raw.id = 1;
  raw.open = 10.0;
  raw.close = 12.0;
  raw.high = 15.0;
  raw.low = 8.0;
  raw.vol = 100.0;
  raw.amount = 1000.0;

  auto nb = czsc::NewBar::FromRaw(raw);
  EXPECT_EQ(nb.symbol, raw.symbol);
  EXPECT_EQ(nb.id, raw.id);
  EXPECT_EQ(nb.elements.size(), 1u);
  EXPECT_EQ(nb.elements[0], raw);
}

TEST(NewBarTest, Equality) {
  auto a = czsc::NewBar::FromRaw(czsc::RawBar{});
  auto b = a;
  EXPECT_TRUE(a == b);

  b.elements.clear();
  EXPECT_TRUE(a != b);
}

TEST(NewBarTest, JsonRoundTrip) {
  czsc::RawBar raw;
  raw.symbol = "test";
  raw.dt = 1000;
  raw.open = 1.0;
  auto nb = czsc::NewBar::FromRaw(raw);

  nlohmann::json j = nb;
  auto nb2 = j.get<czsc::NewBar>();
  EXPECT_EQ(nb, nb2);
}

// ============================================================
// Freq 枚举测试（对齐 freq.rs:365-422）
// ============================================================
TEST(FreqTest, FromString) {
  EXPECT_EQ(czsc::FreqFromString("F5"), czsc::Freq::k5Min);
  EXPECT_EQ(czsc::FreqFromString("D"), czsc::Freq::kDay);
  EXPECT_EQ(czsc::FreqFromString("W"), czsc::Freq::kWeek);
  EXPECT_EQ(czsc::FreqFromString("M"), czsc::Freq::kMonth);
  EXPECT_EQ(czsc::FreqFromString("F1"), czsc::Freq::k1Min);
  EXPECT_EQ(czsc::FreqFromString("Tick"), czsc::Freq::kTick);
  // 中文别名
  EXPECT_EQ(czsc::FreqFromString("5分钟"), czsc::Freq::k5Min);
  EXPECT_EQ(czsc::FreqFromString("日线"), czsc::Freq::kDay);
}

TEST(FreqTest, Name) {
  EXPECT_STREQ(czsc::FreqName(czsc::Freq::k5Min), "F5");
  EXPECT_STREQ(czsc::FreqName(czsc::Freq::kDay), "D");
  EXPECT_STREQ(czsc::FreqName(czsc::Freq::kWeek), "W");
}

TEST(FreqTest, IsMinute) {
  EXPECT_TRUE(czsc::FreqIsMinute(czsc::Freq::k5Min));
  EXPECT_TRUE(czsc::FreqIsMinute(czsc::Freq::k60Min));
  EXPECT_FALSE(czsc::FreqIsMinute(czsc::Freq::kDay));
  EXPECT_FALSE(czsc::FreqIsMinute(czsc::Freq::kWeek));
}

TEST(FreqTest, Minutes) {
  EXPECT_EQ(czsc::FreqMinutes(czsc::Freq::k5Min), 5);
  EXPECT_EQ(czsc::FreqMinutes(czsc::Freq::k60Min), 60);
  EXPECT_EQ(czsc::FreqMinutes(czsc::Freq::kDay), -1);   // 非分钟
}

// ============================================================
// Direction / Mark 枚举测试
// ============================================================
TEST(DirectionTest, Names) {
  EXPECT_STREQ(czsc::DirectionName(czsc::Direction::kUp), "Up");
  EXPECT_STREQ(czsc::DirectionName(czsc::Direction::kDown), "Down");
}

TEST(MarkTest, Names) {
  EXPECT_STREQ(czsc::MarkName(czsc::Mark::kD), "D");
  EXPECT_STREQ(czsc::MarkName(czsc::Mark::kG), "G");
}

// ============================================================
// FX 分型测试（对齐 fx.rs:316-401）
// ============================================================
namespace {

czsc::NewBar MakeTestBar(int id, double o, double c, double h, double l,
                         double v = 100.0, double a = 1000.0) {
  czsc::NewBar nb;
  nb.symbol = "TEST";
  nb.id = id;
  nb.open = o;
  nb.close = c;
  nb.high = h;
  nb.low = l;
  nb.vol = v;
  nb.amount = a;
  return nb;
}

czsc::FX MakeTestFX(czsc::Mark mark) {
  czsc::FX fx;
  fx.symbol = "TEST";
  fx.mark = mark;
  if (mark == czsc::Mark::kD) {
    // 底分型：中间K线最低
    fx.elements.push_back(MakeTestBar(1, 8.5, 8.2, 9.0, 8.0, 90.0, 900.0));
    fx.elements.push_back(MakeTestBar(2, 8.0, 8.0, 8.5, 7.5, 100.0, 1000.0));
    fx.elements.push_back(MakeTestBar(3, 8.5, 8.8, 9.0, 8.0, 110.0, 1100.0));
    fx.high = 8.5;
    fx.low = 7.5;
    fx.fx = 7.5;
  } else {
    // 顶分型：中间K线最高
    fx.elements.push_back(MakeTestBar(1, 10.0, 11.0, 11.5, 9.5, 90.0, 900.0));
    fx.elements.push_back(MakeTestBar(2, 11.0, 11.5, 12.0, 10.5, 100.0, 1000.0));
    fx.elements.push_back(MakeTestBar(3, 11.5, 10.5, 11.8, 10.0, 110.0, 1100.0));
    fx.high = 12.0;
    fx.low = 10.5;
    fx.fx = 12.0;
  }
  return fx;
}

}  // namespace

TEST(FXTest, DPowerStr) {
  auto fx = MakeTestFX(czsc::Mark::kD);
  // k3.close=8.8, k1.high=9.0, k2.high=8.5
  // 8.8 > 8.5 → "中"
  EXPECT_STREQ(fx.power_str(), "\xe4\xb8\xad");  // 中
}

TEST(FXTest, GPowerStrWeak) {
  auto fx = MakeTestFX(czsc::Mark::kG);
  // k3.close=10.5, k1.low=9.5, k2.low=10.5
  // 10.5 < 9.5? No. 10.5 < 10.5? No (equal, not less)
  // → "弱"
  EXPECT_STREQ(fx.power_str(), "\xe5\xbc\xb1");  // 弱
}

TEST(FXTest, GPowerStrStrong) {
  auto fx = MakeTestFX(czsc::Mark::kG);
  // 调 k3.close 使其穿过 k1.low
  fx.elements[2].close = 9.0;  // < k1.low(9.5) → "强"
  EXPECT_STREQ(fx.power_str(), "\xe5\xbc\xba");  // 强
}

TEST(FXTest, PowerVolume) {
  auto fx = MakeTestFX(czsc::Mark::kD);
  EXPECT_DOUBLE_EQ(fx.power_volume(), 300.0);  // 90+100+110
}

TEST(FXTest, HasZS) {
  auto fx = MakeTestFX(czsc::Mark::kD);
  // zd = max(lows) = max(8.0,7.5,8.0) = 8.0
  // zg = min(highs) = min(9.0,8.5,9.0) = 8.5
  // 8.5 >= 8.0 → true
  EXPECT_TRUE(fx.has_zs());
}

TEST(FXTest, JsonRoundTrip) {
  auto fx = MakeTestFX(czsc::Mark::kD);
  nlohmann::json j = fx;
  auto fx2 = j.get<czsc::FX>();
  EXPECT_EQ(fx, fx2);
}

// ============================================================
// BI 笔测试（对齐 bi.rs:580-607）
// ============================================================
TEST(BITest, PowerPrice) {
  czsc::BI bi;
  bi.fx_a.fx = 10.0;
  bi.fx_b.fx = 15.0;
  EXPECT_DOUBLE_EQ(bi.get_power_price(), 5.0);
}

TEST(BITest, PowerPriceZero) {
  czsc::BI bi;
  bi.fx_a.fx = 10.0;
  bi.fx_b.fx = 10.0;
  EXPECT_DOUBLE_EQ(bi.get_power_price(), 0.0);
}

TEST(BITest, PowerVolume) {
  czsc::BI bi;
  // <= 2 bars → 0
  bi.bars.resize(2);
  EXPECT_DOUBLE_EQ(bi.get_power_volume(), 0.0);

  // 4 bars: sum bars[1].vol + bars[2].vol
  bi.bars.resize(4);
  bi.bars[1].vol = 50.0;
  bi.bars[2].vol = 30.0;
  EXPECT_DOUBLE_EQ(bi.get_power_volume(), 80.0);
}

TEST(BITest, Change) {
  czsc::BI bi;
  bi.fx_a.fx = 10.0;
  bi.fx_b.fx = 12.5;
  // (12.5-10.0)/10.0 = 0.25
  EXPECT_DOUBLE_EQ(bi.get_change(), 0.25);
}

TEST(BITest, ChangeZeroFx) {
  czsc::BI bi;
  bi.fx_a.fx = 0.0;
  bi.fx_b.fx = 10.0;
  EXPECT_DOUBLE_EQ(bi.get_change(), 0.0);
}

TEST(BITest, SNR) {
  czsc::BI bi;
  // 构建 raw_bars: 3 个 bar 使得 bars > 2
  czsc::NewBar nb1, nb2, nb3;
  nb1.open = 10.0; nb1.close = 11.0;
  nb2.open = 11.0; nb2.close = 10.5;
  nb3.open = 10.5; nb3.close = 12.0;
  // 在 elements 中放 RawBar 使 get_raw_bars 有内容
  czsc::RawBar r1, r2, r3;
  r1.open = 10.0; r1.close = 11.0;
  r2.open = 11.0; r2.close = 10.5;
  r3.open = 10.5; r3.close = 12.0;
  nb1.elements.push_back(r1);
  nb2.elements.push_back(r2);
  nb3.elements.push_back(r3);
  bi.bars = {nb1, nb2, nb3};
  // get_raw_bars: bars[1].elements = [r2]
  //               bars = {nb1,nb2,nb3}, 只取 [1..2) = {nb2}
  // n=1 → snr = |close-open| = |10.5-11.0| = 0.5
  EXPECT_DOUBLE_EQ(bi.get_snr(), 0.5);
}

TEST(BITest, Slope) {
  czsc::BI bi;
  czsc::NewBar nb1, nb2, nb3;
  czsc::RawBar r1, r2, r3;
  r1.close = 10.0; nb1.elements.push_back(r1);
  r2.close = 12.0; nb2.elements.push_back(r2);
  r3.close = 14.0; nb3.elements.push_back(r3);
  bi.bars = {nb1, nb2, nb3};
  // raw_bars: [r2], only 1 element, < 2 → slope=0
  EXPECT_DOUBLE_EQ(bi.get_slope(), 0.0);

  // Add more bars to get 2 raw bars
  czsc::NewBar nb4;
  czsc::RawBar r4;
  r4.close = 16.0; nb4.elements.push_back(r4);
  bi.bars.push_back(nb4);
  // raw_bars: [r2, r3] — 2 elements
  // x_mean=0.5, y_mean=14.0
  // numerator = (-0.5)*(12-14) + (0.5)*(14-14) = 1 + 0 = 1
  // denominator = 0.25 + 0.25 = 0.5
  // slope = 2.0
  EXPECT_DOUBLE_EQ(bi.get_slope(), 2.0);
}

TEST(BITest, HighLow) {
  czsc::BI bi;
  bi.fx_a.high = 15.0;
  bi.fx_a.low = 8.0;
  bi.fx_b.high = 12.0;
  bi.fx_b.low = 9.0;
  EXPECT_DOUBLE_EQ(bi.get_high(), 15.0);
  EXPECT_DOUBLE_EQ(bi.get_low(), 8.0);
}

TEST(BITest, DefaultGetters) {
  czsc::BI bi;
  EXPECT_EQ(bi.get_length(), 0u);
  EXPECT_DOUBLE_EQ(bi.get_rsq(), 0.0);  // Phase 1 placeholder
}

TEST(BITest, JsonRoundTrip) {
  czsc::BI bi;
  bi.fx_a = MakeTestFX(czsc::Mark::kD);
  bi.fx_b = MakeTestFX(czsc::Mark::kG);
  bi.direction = czsc::Direction::kUp;

  nlohmann::json j = bi;
  auto bi2 = j.get<czsc::BI>();
  EXPECT_EQ(bi, bi2);
}

// ============================================================
// ZS 中枢测试
// ============================================================
namespace {

czsc::BI MakeTestBI(double lo, double hi, czsc::Direction dir) {
  czsc::BI bi;
  bi.direction = dir;
  bi.fx_a.high = hi;
  bi.fx_a.low = lo;
  bi.fx_a.fx = (dir == czsc::Direction::kUp) ? lo : hi;
  bi.fx_b.high = hi + 2;
  bi.fx_b.low = lo + 2;
  bi.fx_b.fx = (dir == czsc::Direction::kUp) ? hi + 2 : lo + 2;
  return bi;
}

}  // namespace

TEST(ZSTest, ConstructFromBis) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeTestBI(8.0, 12.0, czsc::Direction::kUp));    // 向上笔
  bis.push_back(MakeTestBI(9.0, 11.0, czsc::Direction::kDown));  // 向下笔
  bis.push_back(MakeTestBI(8.5, 12.5, czsc::Direction::kUp));    // 向上笔

  czsc::ZS zs(bis);

  // zg = min(12.0, 11.0, 12.5) = 11.0  -- wait, that's min of high
  // Actually zg = min(highs of first 3) = min(12, 11, 12.5) = 11.0
  // zd = max(lows of first 3) = max(8, 9, 8.5) = 9.0
  // Actually these are get_high() and get_low() which are max/min of fx_a and fx_b
  // Let me recalculate...
  // bi[0]: fx_a.high=12, fx_a.low=8, fx_b.high=14, fx_b.low=10 → get_high()=14, get_low()=8
  // bi[1]: fx_a.high=11, fx_a.low=9, fx_b.high=13, fx_b.low=11 → get_high()=13, get_low()=9
  // bi[2]: fx_a.high=12.5, fx_a.low=8.5, fx_b.high=14.5, fx_b.low=10.5 → get_high()=14.5, get_low()=8.5
  // zg = min(14, 13, 14.5) = 13.0
  // zd = max(8, 9, 8.5) = 9.0
  EXPECT_DOUBLE_EQ(zs.zg, 13.0);
  EXPECT_DOUBLE_EQ(zs.zd, 9.0);
  EXPECT_DOUBLE_EQ(zs.zz, 11.0);  // 9 + (13-9)*0.5 = 11
  // gg = max(14,13,14.5) = 14.5, dd = min(8,9,8.5) = 8
  EXPECT_DOUBLE_EQ(zs.gg, 14.5);
  EXPECT_DOUBLE_EQ(zs.dd, 8.0);
  EXPECT_EQ(zs.sdir, czsc::Direction::kUp);
}

TEST(ZSTest, IsValid) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeTestBI(8.0, 12.0, czsc::Direction::kUp));
  bis.push_back(MakeTestBI(9.0, 11.0, czsc::Direction::kDown));
  bis.push_back(MakeTestBI(8.5, 12.5, czsc::Direction::kUp));

  czsc::ZS zs(bis);
  EXPECT_TRUE(zs.is_valid());
}

TEST(ZSTest, Empty) {
  czsc::ZS zs;
  EXPECT_FALSE(zs.is_valid());
  EXPECT_EQ(zs.bis.size(), 0u);
}

TEST(ZSTest, JsonRoundTrip) {
  std::vector<czsc::BI> bis;
  bis.push_back(MakeTestBI(8.0, 12.0, czsc::Direction::kUp));
  bis.push_back(MakeTestBI(9.0, 11.0, czsc::Direction::kDown));
  bis.push_back(MakeTestBI(8.5, 12.5, czsc::Direction::kUp));

  czsc::ZS zs(bis);
  nlohmann::json j = zs;
  auto zs2 = j.get<czsc::ZS>();
  EXPECT_EQ(zs, zs2);
}

// ============================================================
// Signal 信号测试（对齐 signal.rs 逻辑）
// ============================================================
TEST(SignalTest, FromString) {
  auto sig = czsc::Signal::FromString("60分钟_D1单K_BS辅助V230506_第3层_任意_任意_0");
  EXPECT_EQ(sig.k1, "60分钟");
  EXPECT_EQ(sig.k2, "D1单K");
  EXPECT_EQ(sig.k3, "BS辅助V230506");
  EXPECT_EQ(sig.v1, "第3层");
  EXPECT_EQ(sig.score, 0);
}

TEST(SignalTest, DefaultSignal) {
  auto sig = czsc::Signal::FromString(
      "\xe4\xbb\xbb\xe6\x84\x8f"
      "_\xe4\xbb\xbb\xe6\x84\x8f"
      "_\xe4\xbb\xbb\xe6\x84\x8f"
      "_\xe4\xbb\xbb\xe6\x84\x8f"
      "_\xe4\xbb\xbb\xe6\x84\x8f"
      "_\xe4\xbb\xbb\xe6\x84\x8f_0");
  EXPECT_TRUE(sig.key().empty());   // 全"任意" → 空 key
}

TEST(SignalTest, Key) {
  auto sig = czsc::Signal::FromString("60分钟_D1_BS_V1_V2_V3_50");
  EXPECT_EQ(sig.key(), "60分钟_D1_BS");
  EXPECT_EQ(sig.value(), "V1_V2_V3_50");
}

TEST(SignalTest, InvalidFormat) {
  EXPECT_THROW(czsc::Signal::FromString("too_few"), std::invalid_argument);
  EXPECT_THROW(czsc::Signal::FromString("1_2_3_4_5_6_101"), std::invalid_argument);  // score > 100
}

TEST(SignalTest, IsMatch) {
  auto sig = czsc::Signal::FromString("60分钟_D1_BS_V1_V2_V3_50");
  std::unordered_map<std::string, std::string> dict;
  dict["60分钟_D1_BS"] = "V1_V2_V3_60";  // score 60 >= 50 → match
  EXPECT_TRUE(sig.is_match(dict));

  dict["60分钟_D1_BS"] = "V1_V2_V3_40";  // score 40 < 50 → no match
  EXPECT_FALSE(sig.is_match(dict));

  // key not found
  std::unordered_map<std::string, std::string> empty;
  EXPECT_FALSE(sig.is_match(empty));
}

TEST(SignalTest, AnyMatch) {
  // v1="任意" → any value matches
  auto sig = czsc::Signal::FromString(
      std::string("60分钟_D1_BS_") + czsc::kSignalAny + "_V2_V3_50");
  std::unordered_map<std::string, std::string> dict;
  dict["60分钟_D1_BS"] = "Different_V2_V3_60";
  EXPECT_TRUE(sig.is_match(dict));
}

TEST(SignalTest, Equality) {
  auto a = czsc::Signal::FromString("A_B_C_D_E_F_50");
  auto b = czsc::Signal::FromString("A_B_C_D_E_F_50");
  auto c = czsc::Signal::FromString("A_B_C_D_E_F_60");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(SignalTest, JsonRoundTrip) {
  auto sig = czsc::Signal::FromString("60分钟_D1_BS_V1_V2_V3_50");
  nlohmann::json j = sig;
  EXPECT_EQ(j.get<std::string>(), sig.to_string());

  auto sig2 = j.get<czsc::Signal>();
  EXPECT_EQ(sig, sig2);
}

TEST(SignalTest, ScoreBoundary) {
  // score=0 边界
  EXPECT_NO_THROW(czsc::Signal::FromString("A_B_C_D_E_F_0"));
  // score=100 边界
  EXPECT_NO_THROW(czsc::Signal::FromString("A_B_C_D_E_F_100"));
}
