// tdx czsc 子命令：批量缠论分析 TDengine 标的，信号写 TDengine 或 shm。
//
// 由 tools/czsc_cli.cpp 合并入 tdx 主 CLI（tdx czsc [选项]），消除独立二进制。
// 代码格式：统一 sh/sz/bj/hk 前缀（sh999999/sz000001/hk00700），裸码拒绝；港股跳过。
//
// 两种写入模式：
//   默认（TDengine 入库）: signals 稳定子表，覆盖式写入。
//   --shm <path>         : 盘中实时模式——信号写共享内存（只写触发态 + 增量计算），
//                          配合 czsc-signals --shm 实时展示。
//
// 用法：
//   tdx czsc [--blk <path>] [--all] [--codes a,b,c] [--freqs F5,F30,D,week]
//            [--n <workers>] [--interval <秒>] [--host/--user/--pass/--db/--port]
//            [--dry-run]
//   tdx czsc --shm /dev/shm/tdx_signals.shm --codes sh600000 --interval 30   （盘中实时）
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

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <nlohmann/json.hpp>

#include "czsc/czsc.h"
#include "czsc/io/data_loader.hpp"
#include "czsc/io/signal_store.hpp"
#include "czsc/types/market.hpp"

#include "tdx/taos/taos_connection.hpp"  // 统一连接 RAII
#include "tdx/shm/signal_shm.hpp"       // 信号 shm 写入器（盘中实时模式）

using json = nlohmann::json;
namespace cs = czsc::signals;
namespace cz = czsc;
namespace io = czsc::io;
using io::CodeKey;

// ---- czsc 子命令专属 flag（ABSL_FLAG 全局注册，对齐 fetch-quotes 模式）----
ABSL_FLAG(std::string, czsc_blk, "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk",
          "通达信 blk 路径（1=sh+6 位 / 0=sz+6 位）");
ABSL_FLAG(bool, czsc_all, false, "扫 stock_name 全市场（忽略 blk）");
ABSL_FLAG(std::string, czsc_codes, "", "逗号分隔代码（须 sh/sz/bj/hk 前缀；港股跳过）");
ABSL_FLAG(std::string, czsc_freqs, "F5,F30,D,week", "周期子集");
ABSL_FLAG(int32_t, czsc_n, 4, "并行工作线程");
ABSL_FLAG(int32_t, czsc_interval, 0, "循环间隔秒（0=单次）");
ABSL_FLAG(std::string, czsc_host, "", "TDengine host（省略走环境变量）");
ABSL_FLAG(std::string, czsc_user, "", "TDengine user");
ABSL_FLAG(std::string, czsc_pass, "", "TDengine pass");
ABSL_FLAG(std::string, czsc_db, "", "TDengine db");
ABSL_FLAG(int32_t, czsc_port, 0, "TDengine port（0=默认）");
ABSL_FLAG(bool, czsc_dry, false, "仅分析不写 DB");
ABSL_FLAG(std::string, czsc_shm, "", "信号 shm 路径（非空=盘中实时模式：增量计算+只写触发态）");
ABSL_FLAG(bool, czsc_incremental, false, "增量计算（--shm 自动启用）");
ABSL_FLAG(int32_t, czsc_shm_slots, 65536, "shm ring buffer 槽数（默认 64K ≈ 4MB）");

namespace {

// 优雅退出信号（SIGINT/SIGTERM）
std::atomic<bool> g_running{true};
void OnSignal(int) { g_running.store(false, std::memory_order_release); }

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

struct Args {
  std::string blk_path = absl::GetFlag(FLAGS_czsc_blk);
  bool all_market = absl::GetFlag(FLAGS_czsc_all);
  std::vector<CodeKey> codes;
  std::vector<czsc::Freq> freqs;
  int n_workers = absl::GetFlag(FLAGS_czsc_n);
  int interval_sec = absl::GetFlag(FLAGS_czsc_interval);
  io::LoaderConfig cfg;
  bool dry_run = absl::GetFlag(FLAGS_czsc_dry);
};

// 逗号分隔字符串 → vector<T>
template <typename F>
auto SplitParse(const std::string& v, F parse) {
  std::vector<decltype(parse(std::string{}))> out;
  size_t p = 0;
  while (p < v.size()) {
    size_t c = v.find(',', p);
    std::string tok = (c == std::string::npos) ? v.substr(p) : v.substr(p, c - p);
    out.push_back(parse(tok));
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return out;
}

Args BuildArgs() {
  Args a;
  a.cfg = io::LoaderConfig::FromEnv();
  if (auto h = absl::GetFlag(FLAGS_czsc_host); !h.empty()) a.cfg.taos_host = h;
  if (auto u = absl::GetFlag(FLAGS_czsc_user); !u.empty()) a.cfg.taos_user = u;
  if (auto p = absl::GetFlag(FLAGS_czsc_pass); !p.empty()) a.cfg.taos_pass = p;
  if (auto d = absl::GetFlag(FLAGS_czsc_db); !d.empty()) a.cfg.taos_db = d;
  if (int pt = absl::GetFlag(FLAGS_czsc_port); pt > 0) a.cfg.taos_port = static_cast<uint16_t>(pt);

  a.codes = SplitParse(absl::GetFlag(FLAGS_czsc_codes),
                       [](const std::string& s) -> CodeKey {
                         auto k = io::NormalizeCode(s);
                         return k ? *k : CodeKey{};
                       });
  a.codes.erase(std::remove_if(a.codes.begin(), a.codes.end(),
                                [](const CodeKey& k) { return k.code.empty(); }),
                 a.codes.end());
  a.freqs = SplitParse(absl::GetFlag(FLAGS_czsc_freqs),
                       [](const std::string& s) { return czsc::FreqFromString(s); });
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
  size_t bars_new = 0;     // 增量：本轮新增 bar 数
  size_t sigs_out = 0;
  size_t sigs_triggered = 0;  // 触发态（score!=0）
  int64_t rows_written = 0;
  int64_t last_ts = 0;     // 最新 bar 时间（增量模式回传）
  std::string error;
};

// 增量引擎状态：按 (code, freq) 跨轮保留。
struct EngineState {
  std::unique_ptr<cz::analyze::CZSC> engine;
  std::unique_ptr<cz::ta::TaCache> cache;
  int64_t last_ts = 0;     // 已处理的最大 bar 时间
  bool initialized = false;
};

// XXH64 简化版（信号名 → 64-bit hash，供 shm 消费者还原）。
static uint64_t HashSignalName(const char* s) {
  uint64_t h = 14695981039346656037ULL;
  for (const char* p = s; *p; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 1099511628211ULL;
  }
  return h ? h : 1;  // 0 保留为"无效"
}

// 加载 K 线（全量或增量）。
static std::vector<cz::RawBar> LoadBars(const std::string& code, czsc::Freq freq,
                                         const io::LoaderConfig& cfg, czsc::Market mkt) {
  if (freq == cz::Freq::kDay) return io::LoadDailyBars(code, cfg, mkt);
  if (freq == cz::Freq::k5Min) return io::Load5mBars(code, cfg, mkt);
  if (freq == cz::Freq::k30Min) return io::Resample5mTo30m(io::Load5mBars(code, cfg, mkt));
  if (freq == cz::Freq::kWeek) return io::ResampleDailyToWeek(io::LoadDailyBars(code, cfg, mkt));
  return {};
}

void RunOneFreq(const io::CodeKey& key, czsc::Freq freq, const Args& args,
                const io::LoaderConfig& cfg,
                const std::function<void(const FreqResult&)>& done,
                tdx::shm::SignalShmWriter* shm_writer,
                EngineState* state) {
  FreqResult r;
  r.display = key.display;
  r.freq = freq;
  std::string freq_tag = io::FreqTag(freq);
  const czsc::Market mkt = czsc::PrefixToMarket(key.mkt);

  // 加载 K 线（全量；增量模式后续按 last_ts 过滤）。
  std::vector<cz::RawBar> src = LoadBars(key.code, freq, cfg, mkt);
  r.bars_in = src.size();
  if (src.size() < 6) { r.error = "bars<6，跳过"; done(r); return; }

  // 增量：分离新 bar（ts > last_ts）。
  std::vector<cz::RawBar> new_bars;
  bool incremental = (state != nullptr && state->initialized);
  if (incremental) {
    for (auto& b : src) {
      if (b.dt > state->last_ts) new_bars.push_back(std::move(b));
    }
    r.bars_new = new_bars.size();
    if (new_bars.empty()) {
      // 无新 bar：跳过计算，仍回传 last_ts。
      r.last_ts = state->last_ts;
      done(r);
      return;
    }
    // 用新 bar 增量更新引擎。
    for (auto& b : new_bars) state->engine->update_bar(b);
    r.last_ts = new_bars.back().dt;
  } else {
    // 首轮：全量创建引擎。
    if (!state) {
      // 无状态模式（TDengine 入库）：临时引擎。
      cz::analyze::CZSC czsc(src, 50, 6);
      cz::ta::TaCache cache;
      auto& reg = cs::signal_registry();
      std::unordered_map<std::string, json> params;
      params["market"] = key.mkt;
      params["di"] = 1;
      cs::ParamView pv(params);
      std::vector<cz::Signal> all;
      all.reserve(reg.size() * 2);
      for (auto& [name, meta] : reg) {
        try {
          for (auto& s : meta.func(czsc, pv, &cache)) all.push_back(std::move(s));
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[%s/%s] 信号异常 %s: %s\n",
                       r.display.c_str(), freq_tag.c_str(), name, e.what());
        }
      }
      r.sigs_out = all.size();
      if (!args.dry_run && !all.empty()) {
        tdx::taos::TaosConnection conn_h{tdx::taos::TaosConfig{cfg.taos_host, cfg.taos_port, cfg.taos_user, cfg.taos_pass, cfg.taos_db}};
        ::TAOS* conn = conn_h.native();
        if (!conn) { r.error = "TDengine 连接失败"; done(r); return; }
        ::taos_query(conn, "USE tdx");
        io::EnsureSignalTable(conn);
        int64_t ts_ms = src.back().dt * 1000;
        r.rows_written = io::RewriteSignals(conn, key.code, key.mkt, freq_tag, all, ts_ms);
        if (r.rows_written < 0) r.error = "写入失败";
      }
      done(r);
      return;
    }
    // 首轮 + 有状态：创建引擎并全量喂入。
    state->engine = std::make_unique<cz::analyze::CZSC>(src, 50, 6);
    state->cache = std::make_unique<cz::ta::TaCache>();
    state->initialized = true;
    r.bars_new = src.size();
    r.last_ts = src.back().dt;
  }

  // 跑信号（增量模式用持久化的 engine/cache）。
  auto& reg = cs::signal_registry();
  std::unordered_map<std::string, json> params;
  params["market"] = key.mkt;
  params["di"] = 1;
  cs::ParamView pv(params);

  std::vector<cz::Signal> all;
  all.reserve(reg.size() * 2);
  for (auto& [name, meta] : reg) {
    try {
      auto sv = meta.func(*state->engine, pv, state->cache.get());
      for (auto& s : sv) all.push_back(std::move(s));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[%s/%s] 信号异常 %s: %s\n",
                   r.display.c_str(), freq_tag.c_str(), name, e.what());
    }
  }
  r.sigs_out = all.size();

  // 只保留触发态：key() 非空 = 至少一个维度非"任意"（有实际信号含义）。
  // 注：score 在所有 make_signal* 构建器中均为 0，不能作为触发判据。
  std::vector<const cz::Signal*> triggered;
  triggered.reserve(all.size());
  for (auto& s : all) {
    if (!s.key().empty()) triggered.push_back(&s);
  }
  r.sigs_triggered = static_cast<size_t>(triggered.size());

  // 写入：shm（盘中实时）或 TDengine（盘后批量）。
  if (shm_writer && !triggered.empty()) {
    uint8_t freq_code = tdx::shm::FreqToCode(freq_tag);
    for (auto* s : triggered) {
      uint64_t nh = HashSignalName(s->key().c_str());
      shm_writer->Push(key.code.c_str(), freq_code, nh,
                       static_cast<double>(s->score),
                       /*signal_ts=*/r.last_ts, /*bar_ts=*/r.last_ts);
    }
    r.rows_written = triggered.size();
  } else if (!shm_writer && !args.dry_run && !all.empty()) {
    tdx::taos::TaosConnection conn_h{tdx::taos::TaosConfig{cfg.taos_host, cfg.taos_port, cfg.taos_user, cfg.taos_pass, cfg.taos_db}};
    ::TAOS* conn = conn_h.native();
    if (!conn) { r.error = "TDengine 连接失败"; done(r); return; }
    ::taos_query(conn, "USE tdx");
    io::EnsureSignalTable(conn);
    int64_t ts_ms = r.last_ts * 1000;
    r.rows_written = io::RewriteSignals(conn, key.code, key.mkt, freq_tag, all, ts_ms);
    if (r.rows_written < 0) r.error = "写入失败";
  }

  if (state) state->last_ts = r.last_ts;
  done(r);
}

// 执行一轮分析，返回 {ok, err, total_sigs, total_written}
struct RoundStats { int64_t ok = 0, err = 0, sigs = 0, triggered = 0, written = 0; };

RoundStats RunRound(const std::vector<CodeKey>& codes, const Args& args,
                    tdx::shm::SignalShmWriter* shm_writer,
                    std::unordered_map<std::string, EngineState>* states) {
  struct Task { CodeKey key; czsc::Freq freq; std::string state_key; };
  std::vector<Task> tasks;
  tasks.reserve(codes.size() * args.freqs.size());
  for (const auto& k : codes)
    for (auto f : args.freqs)
      tasks.push_back({k, f, k.mkt + k.code + "_" + io::FreqTag(f)});

  std::atomic<size_t> idx{0};
  std::mutex io_mu;
  RoundStats st;
  std::unordered_map<std::string, int> err_hist;

  auto worker = [&] {
    while (g_running.load(std::memory_order_acquire)) {
      size_t i = idx.fetch_add(1);
      if (i >= tasks.size()) break;
      const auto& t = tasks[i];
      EngineState* state = states ? &(*states)[t.state_key] : nullptr;
      RunOneFreq(t.key, t.freq, args, args.cfg, [&](const FreqResult& r) {
        std::lock_guard<std::mutex> lk(io_mu);
        if (!r.error.empty()) { ++st.err; err_hist[r.error.substr(0, 40)]++; }
        else ++st.ok;
        st.sigs += r.sigs_out;
        st.triggered += r.sigs_triggered;
        st.written += std::max<int64_t>(0, r.rows_written);
        if (r.rows_written >= 0)
          std::printf("  [%4zu/%-4zu] %-10s %-4s bars=%-5zu+%-3zu sigs=%-4zu(%zu) wr=%lld\n",
                      i + 1, tasks.size(), r.display.c_str(), FreqName(r.freq),
                      r.bars_in, r.bars_new, r.sigs_out, r.sigs_triggered, (long long)r.rows_written);
        else
          std::printf("  [%4zu/%-4zu] %-10s %-4s bars=%-5zu+%-3zu sigs=%-4zu(%zu)\n",
                      i + 1, tasks.size(), r.display.c_str(), FreqName(r.freq),
                      r.bars_in, r.bars_new, r.sigs_out, r.sigs_triggered);
        if (!r.error.empty())
          std::printf("        → %s\n", r.error.c_str());
      }, shm_writer, state);
    }
  };

  auto t0 = std::chrono::steady_clock::now();
  {
    std::vector<std::thread> pool;
    for (int w = 0; w < args.n_workers; ++w) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }
  auto sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  if (shm_writer) {
    std::printf("\n── 本轮 %.1fs  ok=%d err=%d sigs=%lld triggered=%lld shm_written=%lld ──\n",
                sec, (int)st.ok, (int)st.err, (long long)st.sigs,
                (long long)st.triggered, (long long)st.written);
  } else {
    std::printf("\n── 本轮 %.1fs  ok=%d err=%d sigs=%lld written=%lld ──\n",
                sec, (int)st.ok, (int)st.err, (long long)st.sigs, (long long)st.written);
  }
  if (!err_hist.empty()) {
    std::printf("错误分布：\n");
    for (auto& [k, v] : err_hist) std::printf("  %dx %s\n", v, k.c_str());
  }
  return st;
}

}  // namespace

int DoCzsc(int /*argc*/, char** /*argv*/) {
  ::signal(SIGINT, OnSignal);
  ::signal(SIGTERM, OnSignal);

  Args args = BuildArgs();
  std::string shm_path = absl::GetFlag(FLAGS_czsc_shm);
  bool use_shm = !shm_path.empty();
  bool incremental = use_shm || absl::GetFlag(FLAGS_czsc_incremental);

  std::printf("=== tdx czsc  %s ===\n", NowStr().c_str());
  std::printf("TDengine: %s:%u/%s workers=%d interval=%ds dry=%d\n",
              args.cfg.taos_host.c_str(), args.cfg.taos_port, args.cfg.taos_db.c_str(),
              args.n_workers, args.interval_sec, args.dry_run);
  if (use_shm) {
    std::printf("SHM: %s  mode=%s\n", shm_path.c_str(),
                incremental ? "incremental+triggered" : "triggered");
  }

  // 初始化 shm 写入器（盘中实时模式）。
  tdx::shm::SignalShmWriter shm_writer;
  if (use_shm) {
    uint32_t slots = static_cast<uint32_t>(absl::GetFlag(FLAGS_czsc_shm_slots));
    if (!shm_writer.Open(shm_path, slots)) {
      std::fprintf(stderr, "无法打开 shm：%s\n", shm_path.c_str());
      return 1;
    }
    std::printf("SHM 已创建：%s (%u slots, %zu B)\n",
                shm_path.c_str(), shm_writer.slot_count(),
                static_cast<size_t>(shm_writer.slot_count()) * sizeof(tdx::shm::SignalSlot) + 64);
  }

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

  // 增量引擎状态（跨轮保留）。
  std::unordered_map<std::string, EngineState> states;

  int round = 0;
  do {
    ++round;
    std::printf("\n======== 第 %d 轮  %s ========\n", round, NowStr().c_str());
    RunRound(codes, args, use_shm ? &shm_writer : nullptr,
             incremental ? &states : nullptr);
    if (args.interval_sec <= 0) break;
    std::printf("本轮结束，%ds 后再次分析（Ctrl-C 退出）...\n", args.interval_sec);
    for (int s = 0; s < args.interval_sec && g_running.load(std::memory_order_acquire); ++s)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  } while (g_running.load(std::memory_order_acquire));

  std::printf("\ntdx czsc 已退出。\n");
  return 0;
}
