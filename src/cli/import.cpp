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
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <unordered_set>
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
    if (a == "--jobs") { ++i; continue; }
    if (a.rfind("--jobs=", 0) == 0) continue;
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

// 方案 B：Reader 线程（文件 I/O + 重采样）→ Queue → Writer 线程（Appender 写入）。
// Reader 线程零 DB 访问，Writer 独占 Connection + Appender，消除所有写竞态。

struct TableEntry {
  std::string table;
  std::vector<KLine> bars;
  bool replace = false;
};

struct WriteBatch {
  std::string code;
  std::vector<TableEntry> tables;
  std::vector<std::string> state_rows;  // import_state INSERT 值
};

// 增量同步的 last_datetime 缓存：key = code|period。主线程预载后只读共享，reader 线程安全访问。
using LastDtMap = std::unordered_map<std::string, int64_t>;

// 读文件 + 重采样（无 DB 访问），返回 WriteBatch 供 writer 线程消费。
// full=false（增量）时：主表（1d/1m/5m）只写 datetime > last_dt 的新 K 线；
// 派生表（周/月/5m/15m/30m/60m）用完整原始序列重采样后全量 INSERT OR REPLACE（覆盖）。
WriteBatch ReadAndResample(tdx::proto::VipdocReader& reader,
                            const std::string& code, Market market, bool full,
                            const LastDtMap& last_dt) {
  WriteBatch batch;
  batch.code = code;

  auto MaxDt = [](const std::vector<KLine>& bars) -> int64_t {
    int64_t md = 0;
    for (const auto& b : bars) md = std::max(md, b.datetime);
    return md;
  };

  auto AddState = [&](const std::string& period, int64_t dt) {
    char s[384];
    std::snprintf(s, sizeof(s), "('%s', '%s', %lld)", code.c_str(), period.c_str(),
                  static_cast<long long>(dt));
    batch.state_rows.emplace_back(s);
  };

  // 增量过滤：只保留 datetime > last_dt 的 K 线（full 模式不过滤）
  auto FilterNew = [&](std::vector<KLine> bars, const std::string& period) {
    if (full) return bars;
    auto it = last_dt.find(code + "|" + period);
    int64_t cut = (it != last_dt.end()) ? it->second : 0;
    std::vector<KLine> out;
    for (auto& b : bars)
      if (b.datetime > cut) out.push_back(std::move(b));
    return out;
  };

  // ---- 日线 ----
  auto bars_1d = reader.ReadDay(market, code);
  if (!bars_1d.empty()) {
    auto new_1d = FilterNew(bars_1d, "1d");
    if (full || !new_1d.empty()) {
      // 日线：全量全写；增量只写新数据（INSERT OR REPLACE 不覆盖旧行）
      batch.tables.push_back({"kline_1d", full ? bars_1d : new_1d, full});
      // 周/月：完整日线重采样，全量 INSERT OR REPLACE 覆盖（最新周期随新日线变化）
      batch.tables.push_back({"kline_1w", tdx::data::ResampleKline(bars_1d, tdx::data::Freq::Weekly), false});
      batch.tables.push_back({"kline_1mon", tdx::data::ResampleKline(bars_1d, tdx::data::Freq::Monthly), false});
      AddState("1d", MaxDt(bars_1d));
    }
  }

  // ---- 1 分钟线 ----
  auto bars_1m = reader.ReadMin1(market, code);
  if (!bars_1m.empty()) {
    auto new_1m = FilterNew(bars_1m, "1m");
    if (full || !new_1m.empty()) {
      batch.tables.push_back({"kline_1m", full ? bars_1m : new_1m, full});
      batch.tables.push_back({"kline_5m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min5), false});
      batch.tables.push_back({"kline_15m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min15), false});
      batch.tables.push_back({"kline_30m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min30), false});
      batch.tables.push_back({"kline_60m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Hour1), false});
      AddState("1m", MaxDt(bars_1m));
    }
  } else {
    // 5 分钟线（原生，仅在 1m 不可用时）
    auto bars_5m = reader.ReadMin5(market, code);
    if (!bars_5m.empty()) {
      auto new_5m = FilterNew(bars_5m, "5m");
      if (full || !new_5m.empty()) {
        batch.tables.push_back({"kline_5m", full ? bars_5m : new_5m, full});
        batch.tables.push_back({"kline_15m", tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Min15), false});
        batch.tables.push_back({"kline_30m", tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Min30), false});
        batch.tables.push_back({"kline_60m", tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Hour1), false});
        AddState("5m", MaxDt(bars_5m));
      }
    }
  }

  return batch;
}

// MPMC 任务队列：有界队列（max 256），reader 快于 writer 时背压节流
struct TaskQueue {
  static constexpr size_t kMaxQueue = 1024;
  std::deque<WriteBatch> queue;
  std::mutex mutex;
  std::condition_variable cv;       // writer 等待项
  std::condition_variable not_full; // reader 等待空间
  bool closed = false;

  void Push(WriteBatch&& b) {
    {
      std::unique_lock<std::mutex> lk(mutex);
      not_full.wait(lk, [this] { return queue.size() < kMaxQueue || closed; });
      if (closed) return;
      queue.push_back(std::move(b));
    }
    cv.notify_one();
  }

  bool Pop(WriteBatch& b) {
    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [this] { return !queue.empty() || closed; });
    if (queue.empty()) return false;
    b = std::move(queue.front());
    queue.pop_front();
    lk.unlock();
    not_full.notify_one();  // 释放一个写者槽位，唤醒阻塞的 reader
    return true;
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lk(mutex);
      closed = true;
    }
    cv.notify_all();
    not_full.notify_all();  // 唤醒阻塞在 Push 的 reader，使其看到 closed=true 后退出
  }
};

struct ImportStats {
  std::atomic<int64_t> total_bars{0};
  std::atomic<int> code_count{0};
  std::atomic<int> progress{0};
};

// Reader 线程：读 vipdoc → 重采样 → Push 到队列
void ReaderWorker(const std::vector<std::pair<Market, std::string>>& targets,
                  const std::string& tdx_home, bool full,
                  const LastDtMap& last_dt,
                  TaskQueue& queue, ImportStats& stats,
                  int start, int end) {
  tdx::proto::VipdocReader reader(tdx_home);
  for (int i = start; i < end; ++i) {
    const auto& [market, code] = targets[static_cast<size_t>(i)];
    auto batch = ReadAndResample(reader, code, market, full, last_dt);
    int64_t n = 0;
    for (const auto& te : batch.tables) n += te.bars.size();
    queue.Push(std::move(batch));
    if (n > 0) {
      stats.total_bars.fetch_add(n, std::memory_order_relaxed);
      stats.code_count.fetch_add(1, std::memory_order_relaxed);
    }
    stats.progress.fetch_add(1, std::memory_order_relaxed);
  }
}

// Writer 线程：从队列 Pop → Appender 写入 → 周期性 COMMIT
void WriterThread(const std::string& db_path, TaskQueue& queue, int batch_commit) {
  tdx::query::DuckDBQuery db(db_path);
  int since_commit = 0;

  WriteBatch batch;
  while (queue.Pop(batch)) {
    try {
      db.Exec("BEGIN TRANSACTION");
      for (const auto& te : batch.tables) {
        if (te.bars.empty()) continue;
        db.InsertKlines(te.table, te.bars, batch.code, te.replace);
      }
      for (const auto& r : batch.state_rows)
        db.Exec("INSERT OR REPLACE INTO import_state VALUES " + r);
      db.Exec("COMMIT");
    } catch (const std::exception& e) {
      db.Exec("ROLLBACK");
      std::cerr << "\n  write error(" << batch.code << "): " << e.what() << "\n";
    }
    if (++since_commit >= batch_commit) {
      // 周期性 WAL checkpoint（防 WAL 无限增长）
      db.Exec("CHECKPOINT");
      since_commit = 0;
    }
  }
}

// 复权因子：串行获取（单连接，无竞态；通达信 XDXR 限流严重，并行收益低于限流风险）。
// 增量模式：per-code SELECT COUNT 已有则跳过（本地查询，远快于网络）。
// 每个 code 后 sleep 50ms 防限流；连续 3 次错误退避 3 秒。
int ImportAdjustFactors(const std::vector<std::pair<Market, std::string>>& targets,
                         const std::string& db_path, bool full) {
  tdx::query::DuckDBQuery db(db_path);
  // 增量模式：预载 adjust 已有 code 集合（一次查询），per-code O(1) 查找跳过。
  // 不能用 Exec(SELECT COUNT)——它返回 RowCount（COUNT 恒 1 行）而非 count 值。
  std::unordered_set<std::string> existing;
  if (!full) existing = db.LoadAdjustCodes();

  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) {
    std::cerr << "连接失败: " << ec.message() << " — 跳过复权因子\n";
    return 0;
  }

  int n_total = static_cast<int>(targets.size());
  int total_events = 0;
  int consecutive_errors = 0;
  auto t0 = std::chrono::steady_clock::now();

  for (int i = 0; i < n_total; ++i) {
    const auto& [market, code] = targets[static_cast<size_t>(i)];

    // 增量：已有复权因子则跳过（预载集合 O(1) 查找）
    bool skipped = (!full) && existing.count(code);
    if (skipped) consecutive_errors = 0;

    if (!skipped) {
      try {
        auto xdxr_list = sq.GetXdxr(market, code);
        consecutive_errors = 0;
        if (!xdxr_list.empty()) {
          db.Exec("BEGIN TRANSACTION");
          int inserted = 0;  // 检查 Exec 返回值，避免静默写入失败
          for (const auto& x : xdxr_list) {
            char sql[600];
            std::snprintf(sql, sizeof(sql),
                          "INSERT OR REPLACE INTO adjust VALUES "
                          "('%s', '%s', 1.0, %.4f, %.4f, %.4f, %.4f, %d, '%s')",
                          code.c_str(), x.date.c_str(),
                          x.fenhong, x.peigujia, x.songzhuangu, x.peigu,
                          x.category, x.name.c_str());
            if (db.Exec(sql) >= 0) ++inserted;
          }
          db.Exec("COMMIT");
          total_events += inserted;
          if (inserted < static_cast<int>(xdxr_list.size()))
            std::cerr << "\n  " << code << " 复权写入部分失败: " << inserted
                      << "/" << xdxr_list.size() << "（INSERT OR REPLACE 需 UNIQUE 约束）\n";
        }
      } catch (const std::exception& e) {
        // GetXdxr 在事务外，网络失败无需回滚；连续错误退避
        std::cerr << "\n  " << code << " 复权因子获取失败: " << e.what() << "\n";
        if (++consecutive_errors >= 3) {
          std::this_thread::sleep_for(std::chrono::seconds(3));
          consecutive_errors = 0;
        }
      }
      // 防限流：每请求间 50ms
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    int pct = n_total > 0 ? ((i + 1) * 100 / n_total) : 0;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "\r[" << pct << "%] " << (i + 1) << "/" << n_total
              << "  复权事件 " << total_events << " 条  " << elapsed << "s" << std::flush;
  }

  sq.Close();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cout << "\r[100%] " << n_total << "/" << n_total
            << "  复权事件: " << total_events << " 条  " << elapsed << "s\n";
  return total_events;
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
    db.ConfigureForImport();
    const std::vector<std::string> tables = {
        "kline_1m", "kline_5m", "kline_15m", "kline_30m", "kline_60m",
        "kline_1d", "kline_1w", "kline_1mon"
    };
    for (const auto& t : tables) db.EnsureKlineTable(t);
    db.Exec("CREATE TABLE IF NOT EXISTS adjust "
            "(code VARCHAR, date VARCHAR, factor DOUBLE, fenhong DOUBLE, "
            "peigujia DOUBLE, songzhuangu DOUBLE, peigu DOUBLE, "
            "category INTEGER, name VARCHAR)");
    db.Exec("CREATE UNIQUE INDEX IF NOT EXISTS adjust_idx ON adjust (code, date, category)");
    db.Exec("CREATE TABLE IF NOT EXISTS import_state "
            "(code VARCHAR, period VARCHAR, last_datetime BIGINT, "
            "PRIMARY KEY (code, period))");
  }

  // 增量模式：预载 import_state 到 LastDtMap（主线程一次查询，reader 线程只读共享，线程安全）。
  // 全量模式无需预载（不过滤）。key = code|period。
  LastDtMap last_dt_map;
  if (!cfg.full) {
    tdx::query::DuckDBQuery db(cfg.db_path);
    last_dt_map = db.LoadImportState();
  }

  // 3. 方案 B：Reader 线程（读文件+重采样）→ Queue → Writer 线程（Appender 写入，单连接）
  std::cout << "=== 本地数据导入 ===\n";
  TaskQueue queue;
  ImportStats stats;
  std::vector<std::thread> workers;

  // Writer 线程：独占 DuckDB Connection + Appender
  std::thread writer(WriterThread, cfg.db_path, std::ref(queue), /*batch_commit=*/100);

  // Reader 线程：并行读文件 + 重采样，Push 到队列
  int chunk = (n_total + cfg.jobs - 1) / cfg.jobs;
  for (int j = 0; j < cfg.jobs; ++j) {
    int start = j * chunk;
    int end = std::min(start + chunk, n_total);
    if (start >= n_total) break;
    workers.emplace_back(ReaderWorker, std::cref(targets), tdx_home,
                         cfg.full, std::cref(last_dt_map),
                         std::ref(queue), std::ref(stats), start, end);
  }

  // 主线程：进度刷新
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
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 避免 busy-spin 刷屏
  }
  queue.Close();  // 通知 writer 完成
  for (auto& w : workers) w.join();
  writer.join();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cout << "\r[100%] " << n_total << "/" << n_total
            << "  本地导入: " << stats.total_bars.load() << " 根 K 线（"
            << stats.code_count.load() << " 只股票有新数据）  "
            << elapsed << "s\n";

  // 4. 复权因子（并行，每线程独立 StdQuotes 连接 + 50ms 延迟防限流）
  if (cfg.no_adjust) {
    std::cout << "\n跳过复权因子。\n完成。\n";
    return 0;
  }

  std::cout << "\n=== 复权因子（串行 + 50ms 限流） ===\n";
  int n_adjust = ImportAdjustFactors(targets, cfg.db_path, cfg.full);
  std::cout << "\n复权事件: " << n_adjust << " 条\n";

  std::cout << "\n全部完成。数据库: " << cfg.db_path << "\n";
  return 0;
}
