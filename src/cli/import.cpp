// tdx import：TDX 本地历史数据 → DuckDB 导入工具。
//   1. 读取本地 .lc1/.lc5/.day → DuckDB 表（1m/5m/1d）
//   2. 自动重采样 15m/30m/60m/week/mon
//   3. 网络获取复权因子，持久化 + 增量更新
//   4. 全量导入 / 每日增量导入（import_state 表追踪进度）
//   5. 并行导入（-j N / TDX_IMPORT_JOBS=N）
//
// 用法：
//   tdx import --jobs N [full] [codes...]
//     --jobs N      并行线程数（0=CPU 核数，默认 1）
//   环境变量:
//     TDX_HOME         vipdoc 路径
//     TDX_IMPORT_DB    DuckDB 数据库路径
//     TDX_NO_ADJUST=1  跳过复权因子
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/data/resampler.hpp"
#include "tdx/proto/vipdoc_reader.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/query/duckdb_query.hpp"
#include "tdx/types.hpp"
#include "tdx/util/time_util.hpp"

namespace fs = std::filesystem;
using namespace tdx;

namespace {

struct ImportConfig {
  std::string vipdoc_path = "/home/li/.local/share/tdxcfv/drive_c/tc/vipdoc";
  std::string db_path = "./tdx_kline.db";
  bool full = false;
  bool no_adjust = false;
  int jobs = 1;  // 并行线程数，0 = CPU 核数
  std::vector<std::string> codes;
};

ImportConfig ParseArgs(int argc, char** argv, int jobs) {
  ImportConfig cfg;
  const char* home = std::getenv("TDX_HOME");
  if (home) cfg.vipdoc_path = home;
  const char* db = std::getenv("TDX_IMPORT_DB");
  if (db) cfg.db_path = db;
  if (std::getenv("TDX_NO_ADJUST")) cfg.no_adjust = true;
  cfg.jobs = jobs;
  if (cfg.jobs == 0) cfg.jobs = static_cast<int>(std::thread::hardware_concurrency());
  if (cfg.jobs < 1) cfg.jobs = 1;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    // 跳过 absl flag（absl 不自动从 argv 移除）
    if (a == "--import_jobs") { ++i; continue; }
    if (a.rfind("--import_jobs=", 0) == 0) continue;
    if (a == "full") cfg.full = true;
    else if (a == "help") {
      std::cout << "用法: tdx import --jobs N [full] [codes...]\n\n"
                << "  --jobs N        并行线程数（0=CPU 核数，默认 1=串行）\n"
                << "  full            全量导入（默认增量）\n"
                << "  codes...        股票代码（默认扫描 vipdoc 全部）\n\n"
                << "环境变量:\n"
                << "  TDX_HOME         vipdoc 路径（当前: " << cfg.vipdoc_path << "）\n"
                << "  TDX_IMPORT_DB    DuckDB 路径（当前: " << cfg.db_path << "）\n"
                << "  TDX_NO_ADJUST=1  跳过复权因子\n\n"
                << "示例:\n"
                << "  tdx import                         串行增量导入全部\n"
                << "  tdx import --jobs 4 full            4 线程全量导入\n"
                << "  tdx import --jobs 0 600000 000001   全核并行导入指定代码\n";
      std::exit(0);
    } else {
      cfg.codes.push_back(a);
    }
  }
  return cfg;
}

// 扫描 vipdoc 目录下所有 .day 文件对应的代码
// tdx_root: TDX 安装根目录（VipdocReader 拼接 {root}/vipdoc/...）
std::vector<std::pair<Market, std::string>> ScanCodes(const std::string& tdx_root) {
  std::vector<std::pair<Market, std::string>> result;
  for (auto m : {Market::SH, Market::SZ, Market::BJ}) {
    std::string dir = tdx_root + "/vipdoc/" + proto::VipdocReader::MarketDir(m) + "/lday";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file()) continue;
      std::string name = entry.path().filename().string();
      if (name.size() < 5 || name.substr(name.size() - 4) != ".day") continue;
      // 文件名格式：{ex}{code}.day（sh600000.day）
      std::string code = name.substr(2, name.size() - 6);
      if (code.size() != 6) continue;
      bool all_digit = true;
      for (char c : code)
        if (!std::isdigit(static_cast<unsigned char>(c))) { all_digit = false; break; }
      if (!all_digit) continue;
      result.emplace_back(m, code);
    }
  }
  return result;
}

// 导入单个代码的本地 K 线（线程安全：db 为线程独占，reader 只读共享）
int64_t ImportCodeLocal(tdx::query::DuckDBQuery& db,
                         tdx::proto::VipdocReader& reader,
                         const std::string& code, Market market, bool full) {
  int64_t imported = 0;

  // ---- 日线 ----
  auto bars_1d = reader.ReadDay(market, code);
  if (!bars_1d.empty()) {
    int64_t last_dt = full ? 0 : db.LastDatetime("kline_1d", code);
    std::vector<KLine> new_bars;
    for (const auto& b : bars_1d)
      if (b.datetime > last_dt) new_bars.push_back(b);
    if (!new_bars.empty()) {
      db.InsertKlines("kline_1d", new_bars, code, /*replace=*/full);
      imported += new_bars.size();

      auto weekly = tdx::data::ResampleKline(bars_1d, tdx::data::Freq::Weekly);
      auto monthly = tdx::data::ResampleKline(bars_1d, tdx::data::Freq::Monthly);
      db.InsertKlines("kline_1w", weekly, code);
      db.InsertKlines("kline_1mon", monthly, code);
      imported += weekly.size() + monthly.size();
    }
  }

  // ---- 1 分钟线 ----
  auto bars_1m = reader.ReadMin1(market, code);
  if (!bars_1m.empty()) {
    int64_t last_dt = full ? 0 : db.LastDatetime("kline_1m", code);
    std::vector<KLine> new_bars;
    for (const auto& b : bars_1m)
      if (b.datetime > last_dt) new_bars.push_back(b);
    if (!new_bars.empty()) {
      db.InsertKlines("kline_1m", new_bars, code, /*replace=*/full);
      imported += new_bars.size();

      auto m5 = tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min5);
      auto m15 = tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min15);
      auto m30 = tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min30);
      auto h1 = tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Hour1);
      db.InsertKlines("kline_5m", m5, code);
      db.InsertKlines("kline_15m", m15, code);
      db.InsertKlines("kline_30m", m30, code);
      db.InsertKlines("kline_60m", h1, code);
      imported += m5.size() + m15.size() + m30.size() + h1.size();
    }
  }

  // ---- 5 分钟线（原生，仅在 1m 不可用时）----
  if (bars_1m.empty()) {
    auto bars_5m = reader.ReadMin5(market, code);
    if (!bars_5m.empty()) {
      int64_t last_dt = full ? 0 : db.LastDatetime("kline_5m", code);
      std::vector<KLine> new_bars;
      for (const auto& b : bars_5m)
        if (b.datetime > last_dt) new_bars.push_back(b);
      if (!new_bars.empty()) {
        db.InsertKlines("kline_5m", new_bars, code, /*replace=*/full);
        imported += new_bars.size();

        auto m15 = tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Min15);
        auto m30 = tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Min30);
        auto h1 = tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Hour1);
        db.InsertKlines("kline_15m", m15, code);
        db.InsertKlines("kline_30m", m30, code);
        db.InsertKlines("kline_60m", h1, code);
        imported += m15.size() + m30.size() + h1.size();
      }
    }
  }

  // 更新 import_state
  auto UpdateState = [&](const std::string& period, const std::vector<KLine>& bars) {
    if (bars.empty()) return;
    int64_t max_dt = 0;
    for (const auto& b : bars) max_dt = std::max(max_dt, b.datetime);
    char sql[384];
    std::snprintf(sql, sizeof(sql),
                  "INSERT OR REPLACE INTO import_state VALUES ('%s', '%s', %lld)",
                  code.c_str(), period.c_str(), static_cast<long long>(max_dt));
    db.Exec(sql);
  };
  UpdateState("1d", bars_1d);
  UpdateState("1m", bars_1m);
  if (bars_1m.empty()) UpdateState("5m", reader.ReadMin5(market, code));

  return imported;
}

// 并行导入本地数据：线程池分发，每线程独占一个 DuckDB 连接。
// ponytail: 简单 atomic 分片，不用任务队列——文件 I/O 耗时远大于取号开销。
struct ImportStats {
  std::atomic<int64_t> total_bars{0};
  std::atomic<int> code_count{0};
  std::atomic<int> progress{0};
};

void ImportWorker(const std::vector<std::pair<Market, std::string>>& targets,
                  const std::string& tdx_home, tdx::query::DuckDBQuery& db,
                  std::mutex& db_mutex, bool full, ImportStats& stats,
                  int start, int end) {
  tdx::proto::VipdocReader reader(tdx_home);
  for (int i = start; i < end; ++i) {
    const auto& [market, code] = targets[static_cast<size_t>(i)];
    int64_t n = 0;
    {
      std::lock_guard<std::mutex> lk(db_mutex);
      n = ImportCodeLocal(db, reader, code, market, full);
    }
    if (n > 0) {
      stats.total_bars.fetch_add(n, std::memory_order_relaxed);
      stats.code_count.fetch_add(1, std::memory_order_relaxed);
    }
    stats.progress.fetch_add(1, std::memory_order_relaxed);
  }
}

// ponytail: 复权因子进度（串行，简单 printf）
static void AdjProgress(int cur, int total, const std::string& code) {
  int pct = total > 0 ? (cur * 100 / total) : 0;
  std::cout << "\r[" << pct << "%] " << cur << "/" << total << "  " << code
            << " (复权因子)                    " << std::flush;
}

// 网络获取复权因子（增量：仅获取 adjust 表中不存在的 code）
int ImportAdjustFactors(quotes::StdQuotes& sq,
                         tdx::query::DuckDBQuery& db,
                         const std::vector<std::pair<Market, std::string>>& targets,
                         bool full) {
  int total = 0;
  for (size_t i = 0; i < targets.size(); ++i) {
    const auto& [market, code] = targets[i];
    AdjProgress(static_cast<int>(i), static_cast<int>(targets.size()), code);

    // 增量：检查该 code 是否已有复权因子
    if (!full) {
      char check[256];
      std::snprintf(check, sizeof(check),
                    "SELECT COUNT(*) FROM adjust WHERE code = '%s'", code.c_str());
      if (db.Exec(check) > 0) continue;  // 已有，跳过
    }

    try {
      auto xdxr_list = sq.GetXdxr(market, code);
      for (const auto& x : xdxr_list) {
        char sql[600];
        std::snprintf(sql, sizeof(sql),
                      "INSERT OR REPLACE INTO adjust VALUES "
                      "('%s', '%s', 1.0, %.4f, %.4f, %.4f, %.4f, %d, '%s')",
                      code.c_str(), x.date.c_str(),
                      x.fenhong, x.peigujia, x.songzhuangu, x.peigu,
                      x.category, x.name.c_str());
        db.Exec(sql);
        ++total;
      }
    } catch (const std::exception& e) {
      std::cerr << "\n  " << code << " 复权因子获取失败: " << e.what() << "\n";
    }
  }
  return total;
}

}  // namespace

int DoImport(int argc, char** argv, int jobs) {
  auto cfg = ParseArgs(argc, argv, jobs);

  // tdx_home = vipdoc 父目录（VipdocReader 自动拼接 /vipdoc/...）
  std::string tdx_home = cfg.vipdoc_path;
  if (tdx_home.size() > 7 && tdx_home.substr(tdx_home.size() - 7) == "/vipdoc")
    tdx_home = tdx_home.substr(0, tdx_home.size() - 7);
  else if (tdx_home.size() > 7 && tdx_home.substr(tdx_home.size() - 7) == "\\vipdoc")
    tdx_home = tdx_home.substr(0, tdx_home.size() - 7);

  // 1. 确定代码列表
  std::vector<std::pair<Market, std::string>> targets;
  if (!cfg.codes.empty()) {
    for (const auto& c : cfg.codes)
      targets.emplace_back(MarketFromCode(c), c);
  } else {
    targets = ScanCodes(tdx_home);
  }
  if (targets.empty()) {
    std::cerr << "未发现可导入的代码（vipdoc=" << cfg.vipdoc_path << "）\n";
    return 1;
  }
  int n_total = static_cast<int>(targets.size());
  std::cout << "发现 " << n_total << " 只股票\n"
            << "vipdoc: " << cfg.vipdoc_path << "\n"
            << "数据库: " << cfg.db_path << "\n"
            << "并行:   " << cfg.jobs << " 线程\n"
            << "模式:   " << (cfg.full ? "全量导入" : "增量导入") << "\n\n";

  // 2. 初始化数据库表结构
  {
    tdx::query::DuckDBQuery db(cfg.db_path);
    const std::vector<std::string> tables = {
        "kline_1m", "kline_5m", "kline_15m", "kline_30m", "kline_60m",
        "kline_1d", "kline_1w", "kline_1mon"
    };
    for (const auto& t : tables) db.EnsureKlineTable(t);
    db.Exec("CREATE TABLE IF NOT EXISTS adjust "
            "(code VARCHAR, date VARCHAR, factor DOUBLE, fenhong DOUBLE, "
            "peigujia DOUBLE, songzhuangu DOUBLE, peigu DOUBLE, "
            "category INTEGER, name VARCHAR)");
    db.Exec("CREATE INDEX IF NOT EXISTS adjust_idx ON adjust (code, date)");
    db.Exec("CREATE TABLE IF NOT EXISTS import_state "
            "(code VARCHAR, period VARCHAR, last_datetime BIGINT, "
            "PRIMARY KEY (code, period))");
  }

  // 3. 并行导入——共享一个 DuckDB 实例 + mutex 保护
  std::cout << "=== 本地数据导入 ===\n";
  tdx::query::DuckDBQuery db(cfg.db_path);
  std::mutex db_mutex;
  ImportStats stats;
  std::vector<std::thread> workers;
  int chunk = (n_total + cfg.jobs - 1) / cfg.jobs;
  for (int j = 0; j < cfg.jobs; ++j) {
    int start = j * chunk;
    int end = std::min(start + chunk, n_total);
    if (start >= n_total) break;
    workers.emplace_back(ImportWorker, std::cref(targets), tdx_home,
                         std::ref(db), std::ref(db_mutex), cfg.full,
                         std::ref(stats), start, end);
  }

  // 主线程：刷新进度
  auto t0 = std::chrono::steady_clock::now();
  while (true) {
    int done = stats.progress.load(std::memory_order_relaxed);
    if (done >= n_total) break;
    int pct = n_total > 0 ? (done * 100 / n_total) : 0;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "\r[" << pct << "%] " << done << "/" << n_total
              << "  已导入 " << stats.total_bars.load(std::memory_order_relaxed)
              << " 根K线  " << elapsed << "s" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  for (auto& w : workers) w.join();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cout << "\r[100%] " << n_total << "/" << n_total
            << "  本地导入: " << stats.total_bars.load() << " 根 K 线（"
            << stats.code_count.load() << " 只股票有新数据）  "
            << elapsed << "s\n";

  // 4. 复权因子（网络，串行——StdQuotes 内部已有 fiber 池）
  if (cfg.no_adjust) {
    std::cout << "\n跳过复权因子。\n完成。\n";
    return 0;
  }

  std::cout << "\n=== 复权因子 ===\n";
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) {
    std::cerr << "连接失败: " << ec.message() << " — 跳过复权因子\n";
    std::cout << "完成（无复权因子）。\n";
    return 0;
  }

  tdx::query::DuckDBQuery db2(cfg.db_path);
  int n_adjust = ImportAdjustFactors(sq, db2, targets, cfg.full);
  sq.Close();
  std::cout << "\n复权事件: " << n_adjust << " 条\n";

  std::cout << "\n全部完成。数据库: " << cfg.db_path << "\n";
  return 0;
}
