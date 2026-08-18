// tdx czsc-structure 子命令：单只标的单周期 → 缠论结构 JSON。
//
// 供 Python 预测脚本（predict.py）复用 C++ czsc 引擎，不再依赖 Python czsc/talib。
// 输出: bars_raw(全部) + bi_list(笔) + ubi_fxs(未完成笔分型) + bars_ubi(未完成笔)
//       + MACD(dif/dea/hist 末值) + CCI(末值)
//
// 用法:
//   tdx czsc-structure <code> <freq> [--days N]
//     code: sh600519 / sz000001（港股跳过）
//     freq: 1d / 30m / 5m（30m 由 5m 重采样）
//     --days N: 回看天数（默认 400）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"
#include "czsc/io/data_loader.hpp"
#include "czsc/ta/indicators.hpp"
#include "czsc/types/market.hpp"

using json = nlohmann::json;
namespace io = czsc::io;
namespace cz = czsc;

namespace {

// RawBar → JSON
json RawBarToJson(const cz::RawBar& b) {
  return json{
      {"dt", b.dt}, {"o", b.open}, {"h", b.high},
      {"l", b.low}, {"c", b.close}, {"v", b.vol},
  };
}

// FX → JSON（含完整 elements，供 Python 计算影线/成交量）
json NewBarToJson(const cz::NewBar& b) {
  return json{
      {"dt", b.dt}, {"o", b.open}, {"h", b.high},
      {"l", b.low}, {"c", b.close}, {"v", b.vol},
  };
}

json FxToJson(const cz::FX& fx) {
  json elems = json::array();
  for (const auto& e : fx.elements) elems.push_back(NewBarToJson(e));
  return json{
      {"mark", static_cast<int>(fx.mark)},
      {"power", fx.power_str()},
      {"dt", fx.dt}, {"h", fx.high}, {"l", fx.low},
      {"elements", elems},
  };
}

// NewBar → JSON

// 加载 K 线（按 freq）
std::vector<cz::RawBar> LoadBars(const std::string& code, const std::string& freq,
                                  const io::LoaderConfig& cfg, cz::Market mkt) {
  if (freq == "1d") return io::LoadDailyBars(code, cfg, mkt);
  if (freq == "5m") return io::Load5mBars(code, cfg, mkt);
  if (freq == "30m") return io::Resample5mTo30m(io::Load5mBars(code, cfg, mkt));
  return {};
}

}  // namespace

int DoCzscStructure(int argc, char** argv) {
  // 解析位置参数: code freq [--days N]
  std::string code, freq = "1d";
  int days = 400;
  for (int i = 2; i < argc; ++i) {  // argv[0]=程序名 argv[1]=子命令名
    std::string a = argv[i];
    if (a == "--days" && i + 1 < argc) {
      days = std::atoi(argv[++i]);
    } else if (code.empty()) {
      code = a;
    } else if (freq == "1d" && (a == "30m" || a == "5m")) {
      freq = a;
    } else if (a == "1d") {
      freq = a;
    }
  }

  if (code.empty()) {
    std::fprintf(stderr, "用法: tdx czsc-structure <code> <freq> [--days N]\n"
                         "  code: sh600519 / sz000001\n"
                         "  freq: 1d / 30m / 5m\n");
    return 1;
  }

  io::LoaderConfig cfg = io::LoaderConfig::FromEnv();
  cz::Market mkt = cz::PrefixToMarket(code.substr(0, 2));
  if (mkt == cz::Market::kHk) {
    std::fprintf(stderr, "港股不做缠论分析\n");
    return 1;
  }

  auto bars = LoadBars(code.substr(2), freq, cfg, mkt);
  if (bars.size() < 6) {
    std::fprintf(stderr, "K 线不足: %zu 根 (code=%s freq=%s mkt=%d host=%s db=%s)\n",
                 bars.size(), code.c_str(), freq.c_str(), static_cast<int>(mkt),
                 cfg.taos_host.c_str(), cfg.taos_db.c_str());
    return 1;
  }

  cz::analyze::CZSC czsc(bars, 50, 6);

  json j;
  j["symbol"] = code;
  j["freq"] = freq;
  j["bars_raw_count"] = czsc.bars_raw.size();
  j["bars_ubi_count"] = czsc.bars_ubi.size();
  j["bi_count"] = czsc.bi_list.size();

  // 笔列表
  json bi_list = json::array();
  for (const auto& b : czsc.bi_list) {
    bi_list.push_back(json{
        {"direction", b.direction == cz::Direction::kUp ? "向上" : "向下"},
        {"start_dt", b.start_dt()},
        {"end_dt", b.end_dt()},
        {"high", b.get_high()},
        {"low", b.get_low()},
        {"power", b.get_power()},
        {"acceleration", b.get_acceleration()},
        {"rsq", b.get_rsq()},
        {"slope", b.get_slope()},
        {"change", b.get_change()},
        {"fx_a", FxToJson(b.fx_a)},
        {"fx_b", FxToJson(b.fx_b)},
    });
  }
  j["bi_list"] = bi_list;

  // 未完成笔分型
  auto ubi_fxs = czsc.get_ubi_fxs();
  json jfx = json::array();
  for (const auto& fx : ubi_fxs) jfx.push_back(FxToJson(fx));
  j["ubi_fxs"] = jfx;

  // 未完成笔 K 线
  json jubi = json::array();
  for (const auto& b : czsc.bars_ubi) jubi.push_back(NewBarToJson(b));
  j["bars_ubi"] = jubi;

  // 原始 K 线（供 Python 算 MACD/CCI 背离）
  json jraw = json::array();
  size_t start = bars.size() > static_cast<size_t>(days) ? bars.size() - days : 0;
  for (size_t i = start; i < bars.size(); ++i) jraw.push_back(RawBarToJson(bars[i]));
  j["bars_raw"] = jraw;

  // MACD / CCI（C++ ta-lib 计算末值）
  std::vector<double> closes, highs, lows;
  closes.reserve(bars.size());
  highs.reserve(bars.size());
  lows.reserve(bars.size());
  for (const auto& b : bars) {
    closes.push_back(b.close);
    highs.push_back(b.high);
    lows.push_back(b.low);
  }
  // 完整 MACD / CCI 序列（Python 端零指标计算，不依赖 talib）
  auto macd_s = cz::ta::macd(closes.data(), closes.size(), 12, 26, 9);
  auto cci_v = cz::ta::cci(highs.data(), lows.data(), closes.data(), closes.size(), 14);
  if (!macd_s.dif.empty() && !cci_v.empty()) {
    // 只输出与 bars_raw 窗口对齐的末尾段（最近 200 根，足够判断金叉/背离）
    const size_t kTail = 200;
    size_t off = closes.size() > kTail ? closes.size() - kTail : 0;
    j["macd"] = json{
        {"dif", std::vector<double>(macd_s.dif.begin() + off, macd_s.dif.end())},
        {"dea", std::vector<double>(macd_s.dea.begin() + off, macd_s.dea.end())},
        {"hist", std::vector<double>(macd_s.macd.begin() + off, macd_s.macd.end())}};
    j["cci"] = std::vector<double>(cci_v.begin() + off, cci_v.end());
  }

  std::printf("%s\n", j.dump().c_str());
  return 0;
}
