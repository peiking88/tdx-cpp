// czsc_cli：批量缠论分析 TDengine 标的，信号覆盖写入 DB。
//
// 代码格式：统一 sh/sz/bj/hk 前缀（sh999999/sz000001/hk00700），内部转为裸码 + 市场。
//   必须带前缀，裸码一律拒绝；港股(hk)不做缠论分析，cli 自动跳过。
//
// 用法：
//   czsc_cli [选项]
//     --blk <path>    自选股 blk 文件（默认 zxg.blk，标准 7 位：1=sh / 0=sz + 6 位代码）
//     --all           全市场：扫 TDengine stock_name（忽略 blk）
//     --codes a,b,c   显式指定，逗号分隔（须 sh/sz/bj/hk 前缀，如 sh600519,sz002741；港股被跳过）
//     --freqs F5,D,week   周期子集（默认 F5 F30 D week）
//     --n <int>       并行工作线程数（默认 4）
//     --interval <秒> 循环分析：每 N 秒跑一轮（0=单次，默认 0）。优雅退出：Ctrl-C / SIGTERM。
//     --host/--user/--pass/--db/--port   TDengine 连接（省略走 LoaderConfig::FromEnv）
//     --dry-run       仅分析不写 DB。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"
#include "czsc/io/data_loader.hpp"
#include "czsc/io/signal_store.hpp"
#include "czsc/types/market.hpp"

#include "tdx/taos/taos_connection.hpp"  // 统一连接 RAII

using json = nlohmann::json;
namespace cs = czsc::signals;
namespace cz = czsc;
namespace io = czsc::io;
using io::CodeKey;

namespace {

// 优雅退出信号（SIGINT/SIGTERM）
std::atomic<bool> g_running{true};
void OnSignal(int) { g_running.store(false, std::memory_order_release); }

// ---- 工具 ----
std::string TodayStr() {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday);
  return buf;
}
std::string NowStr() {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return buf;
}

const char* FlagVal(int argc, char** argv, int& i, const char* name) {
  std::string s = argv[i];
  auto pos = s.find('=');
  if (pos != std::string::npos) return argv[i] + pos + 1;
  if (i + 1 >= argc) { std::fprintf(stderr, "错误：%s 缺值\n", name); std::exit(2); }
  return argv[++i];
}

struct Args {
  std::string blk_path =
      "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk";
  bool all_market = false;
  std::vector<CodeKey> codes;
  std::vector<czsc::Freq> freqs = {czsc::Freq::k5Min, czsc::Freq::k30Min,
                                  czsc::Freq::kDay, czsc::Freq::kWeek};
  int n_workers = 4;
  int interval_sec = 0;  // 0=单次
  io::LoaderConfig cfg;
  bool dry_run = false;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  a.cfg = io::LoaderConfig::FromEnv();
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s == "--blk")       a.blk_path = FlagVal(argc, argv, i, "--blk");
    else if (s == "--all")  a.all_market = true;
    else if (s == "--codes") {
      std::string v = FlagVal(argc, argv, i, "--codes");
      size_t p = 0;
      while (p < v.size()) {
        size_t c = v.find(',', p);
        std::string tok = (c == std::string::npos) ? v.substr(p) : v.substr(p, c - p);
        if (auto k = io::NormalizeCode(tok)) a.codes.push_back(std::move(*k));
        if (c == std::string::npos) break;
        p = c + 1;
      }
    }
    else if (s == "--freqs") {
      std::string v = FlagVal(argc, argv, i, "--freqs");
      a.freqs.clear();
      size_t p = 0;
      while (p < v.size()) {
        size_t c = v.find(',', p);
        std::string tok = (c == std::string::npos) ? v.substr(p) : v.substr(p, c - p);
        a.freqs.push_back(czsc::FreqFromString(tok));
        if (c == std::string::npos) break;
        p = c + 1;
      }
    }
    else if (s == "--n") a.n_workers = std::atoi(FlagVal(argc, argv, i, "--n"));
    else if (s == "--interval") a.interval_sec = std::atoi(FlagVal(argc, argv, i, "--interval"));
    else if (s == "--host") a.cfg.taos_host = FlagVal(argc, argv, i, "--host");
    else if (s == "--user") a.cfg.taos_user = FlagVal(argc, argv, i, "--user");
    else if (s == "--pass") a.cfg.taos_pass = FlagVal(argc, argv, i, "--pass");
    else if (s == "--db")   a.cfg.taos_db   = FlagVal(argc, argv, i, "--db");
    else if (s == "--port") a.cfg.taos_port = static_cast<uint16_t>(std::atoi(FlagVal(argc, argv, i, "--port")));
    else if (s == "--dry-run") a.dry_run = true;
    else if (s == "-h" || s == "--help") {
      std::printf(
        "czsc_cli 批量缠论信号分析\n"
        "  --blk <path>     通达信 blk（默认 zxg.blk；1=sh+6 位 / 0=sz+6 位）\n"
        "  --all            扫 stock_name（全市场）\n"
        "  --codes a,b,c    逗号分隔（须 sh/sz/bj/hk 前缀，如 sh600519,sz002741；港股跳过）\n"
        "  --freqs F5,F30,D,week   周期子集（默认 4 个）\n"
        "  --n <int>        并行工作线程（默认 4）\n"
        "  --interval <秒>  循环跑：每 N 秒一轮（0=单次，默认 0）\n"
        "  --host/--user/--pass/--db/--port   TDengine 连接\n"
        "  --dry-run        仅分析不写 DB\n");
      std::exit(0);
    }
  }
  return a;
}

// blk → CodeKey（1=sh / 0=sz + 6 位）
std::vector<CodeKey> ReadBlk(const std::string& path) {
  std::vector<CodeKey> out;
  std::ifstream f(path);
  if (!f) { std::fprintf(stderr, "blk 不可读：%s\n", path.c_str()); return out; }
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (line.size() < 7) continue;
    char m = line[0];
    if (m != '0' && m != '1') continue;
    std::string num = line.substr(1, 6);
    if (!std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    CodeKey k;
    k.mkt = (m == '1') ? "sh" : "sz";
    k.code = num;
    k.display = k.mkt + num;
    out.push_back(std::move(k));
  }
  return out;
}

std::vector<CodeKey> ListStockNames(const io::LoaderConfig& cfg) {
  std::vector<CodeKey> out;
  tdx::taos::TaosConnection conn_h{tdx::taos::TaosConfig{cfg.taos_host, cfg.taos_port, cfg.taos_user, cfg.taos_pass, cfg.taos_db}};
  ::TAOS* conn = conn_h.native();
  if (!conn) return out;
  TAOS_RES* res = ::taos_query(conn, "SELECT DISTINCT code FROM stock_name");
  if (res && ::taos_errno(res) == 0) {
    TAOS_ROW row;
    while ((row = ::taos_fetch_row(res))) {
      if (!row[0]) continue;
      std::string c = std::string(reinterpret_cast<const char*>(row[0]));
      if (auto k = io::NormalizeCode(c)) out.push_back(std::move(*k));
    }
  }
  ::taos_free_result(res);
  return out;
}

struct FreqResult {
  std::string display;     // sh600519
  czsc::Freq freq = czsc::Freq::kDay;
  size_t bars_in = 0;
  size_t sigs_out = 0;
  int64_t rows_written = 0;
  std::string error;
};

void RunOneFreq(const io::CodeKey& key, czsc::Freq freq, const Args& args,
                const io::LoaderConfig& cfg,
                const std::function<void(const FreqResult&)>& done) {
  FreqResult r;
  r.display = key.display;
  r.freq = freq;
  std::string freq_tag = io::FreqTag(freq);
  const czsc::Market mkt = czsc::PrefixToMarket(key.mkt);

  std::vector<cz::RawBar> src;
  if (freq == cz::Freq::kDay) {
    src = io::LoadDailyBars(key.code, cfg, mkt);
  } else if (freq == cz::Freq::k5Min) {
    src = io::Load5mBars(key.code, cfg, mkt);
  } else if (freq == cz::Freq::k30Min) {
    src = io::Resample5mTo30m(io::Load5mBars(key.code, cfg, mkt));
  } else if (freq == cz::Freq::kWeek) {
    src = io::ResampleDailyToWeek(io::LoadDailyBars(key.code, cfg, mkt));
  }
  r.bars_in = src.size();
  if (src.size() < 6) { r.error = "bars<6，跳过"; done(r); return; }

  cz::analyze::CZSC czsc(src, 50, 6);
  cz::ta::TaCache cache;

  // ponytail: "market" 通过 ParamView 透传给信号函数，供涨跌停门控。
  auto& reg = cs::signal_registry();
  std::unordered_map<std::string, json> params;
  params["market"] = key.mkt;
  params["di"] = 1;
  cs::ParamView pv(params);

  std::vector<cz::Signal> all;
  all.reserve(reg.size() * 2);
  for (auto& [name, meta] : reg) {
    try {
      auto sv = meta.func(czsc, pv, &cache);
      for (auto& s : sv) all.push_back(std::move(s));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[%s/%s] 信号异常 %s: %s\n",
                   r.display.c_str(), freq_tag.c_str(), name, e.what());
    }
  }
  r.sigs_out = all.size();

  if (!args.dry_run && !all.empty()) {
    tdx::taos::TaosConnection conn_h{tdx::taos::TaosConfig{cfg.taos_host, cfg.taos_port, cfg.taos_user, cfg.taos_pass, cfg.taos_db}};
  ::TAOS* conn = conn_h.native();
    if (!conn) {
      r.error = "TDengine 连接失败";
      done(r);
      return;
    }
    ::taos_query(conn, "USE tdx");
    io::EnsureSignalTable(conn);
    int64_t ts_ms = src.empty() ? 0 : src.back().dt * 1000;
    int64_t n = io::RewriteSignals(conn, key.code, key.mkt, freq_tag, all, ts_ms);
    r.rows_written = n;
    if (n < 0) r.error = "写入失败";
    }
  done(r);
}

// 执行一轮分析，返回 {ok, err, total_sigs, total_written}
struct RoundStats { int64_t ok = 0, err = 0, sigs = 0, written = 0; };

RoundStats RunRound(const std::vector<CodeKey>& codes, const Args& args) {
  struct Task { CodeKey key; czsc::Freq freq; };
  std::vector<Task> tasks;
  tasks.reserve(codes.size() * args.freqs.size());
  for (const auto& k : codes)
    for (auto f : args.freqs)
      tasks.push_back({k, f});

  std::atomic<size_t> idx{0};
  std::mutex io_mu;
  RoundStats st;
  std::unordered_map<std::string, int> err_hist;

  auto worker = [&] {
    while (g_running.load(std::memory_order_acquire)) {
      size_t i = idx.fetch_add(1);
      if (i >= tasks.size()) break;
      const auto& t = tasks[i];
      RunOneFreq(t.key, t.freq, args, args.cfg, [&](const FreqResult& r) {
        std::lock_guard<std::mutex> lk(io_mu);
        if (!r.error.empty()) { ++st.err; err_hist[r.error.substr(0, 40)]++; }
        else ++st.ok;
        st.sigs += r.sigs_out;
        st.written += std::max<int64_t>(0, r.rows_written);
        if (r.rows_written >= 0)
          std::printf("  [%4zu/%-4zu] %-10s %-4s bars=%-5zu sigs=%-4zu wr=%lld\n",
                      i + 1, tasks.size(), r.display.c_str(), FreqName(r.freq),
                      r.bars_in, r.sigs_out, (long long)r.rows_written);
        else
          std::printf("  [%4zu/%-4zu] %-10s %-4s bars=%-5zu sigs=%-4zu\n",
                      i + 1, tasks.size(), r.display.c_str(), FreqName(r.freq),
                      r.bars_in, r.sigs_out);
        if (!r.error.empty())
          std::printf("        → %s\n", r.error.c_str());
      });
    }
  };

  auto t0 = std::chrono::steady_clock::now();
  {
    std::vector<std::thread> pool;
    for (int w = 0; w < args.n_workers; ++w) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }
  auto sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::printf("\n── 本轮 %.1fs  ok=%d err=%d sigs=%lld written=%lld ──\n",
              sec, (int)st.ok, (int)st.err, (long long)st.sigs, (long long)st.written);
  if (!err_hist.empty()) {
    std::printf("错误分布：\n");
    for (auto& [k, v] : err_hist) std::printf("  %dx %s\n", v, k.c_str());
  }
  return st;
}

}  // namespace

int main(int argc, char** argv) {
  ::signal(SIGINT, OnSignal);
  ::signal(SIGTERM, OnSignal);

  Args args = ParseArgs(argc, argv);
  std::printf("=== czsc_cli %s  %s ===\n", PROJECT_VERSION, NowStr().c_str());
  std::printf("TDengine: %s:%u/%s workers=%d interval=%ds dry=%d\n",
              args.cfg.taos_host.c_str(), args.cfg.taos_port, args.cfg.taos_db.c_str(),
              args.n_workers, args.interval_sec, args.dry_run);

  std::vector<CodeKey> codes;
  {
    std::unordered_set<std::string> seen;
    auto add_one = [&](const CodeKey& k) {
      if (k.mkt == "hk") {
        std::printf("  跳过 %s：港股不做缠论分析（仅 mmap 当日快照看盘）\n", k.display.c_str());
        return;
      }
      if (seen.insert(k.mkt + k.code).second) codes.push_back(k);
    };
    if (!args.codes.empty()) {
      for (const auto& k : args.codes) add_one(k);
    }
    // 全市场 or 默认 blk：追加 DB/stock_name 来源
    if (args.all_market || (args.codes.empty())) {
      if (args.all_market) {
        for (auto& k : ListStockNames(args.cfg)) add_one(k);
        std::printf("stock_name 扫入，累计 %zu 只\n", codes.size());
      } else if (args.codes.empty()) {
        auto blk = ReadBlk(args.blk_path);
        std::printf("blk 读入 %zu 行\n", blk.size());
        for (auto& k : blk) add_one(k);
      }
    }
  }

  if (codes.empty()) { std::printf("无可处理标的。\n"); return 0; }
  std::printf("处理标的：%zu 只 × %zu 周期\n", codes.size(), args.freqs.size());

  int round = 0;
  do {
    ++round;
    std::printf("\n======== 第 %d 轮  %s ========\n", round, NowStr().c_str());
    RunRound(codes, args);
    if (args.interval_sec <= 0) break;
    std::printf("本轮结束，%ds 后再次分析（Ctrl-C 退出）...\n", args.interval_sec);
    for (int s = 0; s < args.interval_sec && g_running.load(std::memory_order_acquire); ++s)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  } while (g_running.load(std::memory_order_acquire));

  std::printf("\nczsc_cli 已退出。\n");
  return 0;
}

// PROJECT_VERSION 由 CMake 注入
