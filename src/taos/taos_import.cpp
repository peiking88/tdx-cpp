// TDengine 多线程导入实现。
// 架构：main 线程扫描 vipdoc → 分片 → 启动 N 个 std::thread worker →
//       每 worker 独立 TaosConnection + VipdocReader (+ 可选 StdQuotes)
//       → 读本地文件 + 网络获取复权因子 → 批量 INSERT SQL 写入 TDengine。
// ponytail: 用 raw SQL INSERT 替代 STMT——STMT bind_param_batch 在 helio/absl
// 进程内崩溃（libtaos.so internal memcpy SIGSEGV，独立测试通过），暂不可用。
#include "tdx/taos/taos_import.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/proto/vipdoc_reader.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/types.hpp"
#include "tdx/util/time_util.hpp"

namespace fs = std::filesystem;
using namespace tdx;
using namespace tdx::taos;
using tdx::proto::VipdocReader;

inline constexpr int kBatchSize = 1000;

namespace {

// ---- vipdoc 代码扫描（复用 import.cpp 逻辑）----
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

// 转义 SQL 单引号（ponytail: 股票名含引号概率极低，但安全第一）
std::string EscapeSql(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char ch : s) {
    if (ch == '\'') out += "''";
    else out += ch;
  }
  return out;
}

// 执行 SQL，成功返回 affected rows
int64_t ExecAffected(TAOS* conn, const std::string& sql) {
  TAOS_RES* res = ::taos_query(conn, sql.c_str());
  int code = ::taos_errno(res);
  if (code != 0) {
    std::fprintf(stderr, "TDengine error [%d]: %s\n  SQL(len=%zu)\n",
                 code, ::taos_errstr(res), sql.size());
    ::taos_free_result(res);
    return -1;
  }
  int64_t n = ::taos_affected_rows(res);
  ::taos_free_result(res);
  return n;
}

// 查询子表最新 ts（毫秒），表不存在或无数据返回 0
int64_t LastTimestamp(TAOS* conn, const std::string& tbname) {
  std::string sql = "SELECT MAX(ts) FROM " + tbname;
  TAOS_RES* res = ::taos_query(conn, sql.c_str());
  if (!res || ::taos_errno(res) != 0) {
    ::taos_free_result(res);
    return 0;
  }
  TAOS_ROW row = ::taos_fetch_row(res);
  if (!row || !row[0]) { ::taos_free_result(res); return 0; }
  int64_t val = *static_cast<int64_t*>(row[0]);
  ::taos_free_result(res);
  return val;
}

// ---- 单线程 worker ----
struct Worker {
  int id;
  const ImportTaosConfig& cfg;
  std::vector<std::pair<Market, std::string>> subset;

  int codes_ok = 0;
  int64_t kline_rows = 0;
  int adjust_events = 0;

  void Run() {
    TaosConnection conn(cfg.taos);
    if (!conn) return;

    // USE tdx
    ExecSQL(conn.native(), "USE tdx");

    VipdocReader reader(cfg.vipdoc_path);

    // 复权
    bool have_adjust = !cfg.no_adjust;
    std::unique_ptr<quotes::StdQuotes> sq;
    bool sq_ok = false;
    if (have_adjust) {
      sq = std::make_unique<quotes::StdQuotes>();
      if (!sq->Connect()) sq_ok = true;
      else std::fprintf(stderr, "[w%d] StdQuotes 连接失败，跳过复权\n", id);
    }

    for (const auto& [market, code] : subset) {
      // === K 线 ===
      int64_t n = ImportKlineRaw(conn.native(), reader, market, code, "lday", "day", "1d");
      n += ImportKlineRaw(conn.native(), reader, market, code, "minline", "lc1", "1m");
      n += ImportKlineRaw(conn.native(), reader, market, code, "fzline", "lc5", "5m");
      if (n > 0) { kline_rows += n; ++codes_ok; }

      // === 复权 ===
      if (sq_ok && sq) {
        try {
          auto xdxr = sq->GetXdxr(market, code);
          if (!xdxr.empty()) {
            std::string tb = "a_" + code;
            int64_t last_ts_a = LastTimestamp(conn.native(), tb);
            for (const auto& x : xdxr) {
              int64_t ts = tdx::util::date_to_epoch(
                  std::stoi(x.date.substr(0, 4)),
                  std::stoi(x.date.substr(5, 2)),
                  std::stoi(x.date.substr(8, 2))) * 1000LL;
              if (last_ts_a > 0 && ts <= last_ts_a) continue;  // 增量跳过
              char sql[1024];
              std::snprintf(sql, sizeof(sql),
                "INSERT INTO %s USING adjust TAGS('%s') "
                "VALUES(%lld, %.4f, %.4f, %.4f, %.4f, %d, '%s')",
                tb.c_str(), code.c_str(), (long long)ts,
                x.fenhong, x.peigujia, x.songzhuangu, x.peigu,
                x.category, EscapeSql(x.name).c_str());
              if (ExecAffected(conn.native(), sql) >= 0) ++adjust_events;
            }
          }
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[w%d] 复权失败 %s: %s\n", id, code.c_str(), e.what());
        }
      }
    }
    if (sq) sq->Close();
  }

 private:
  int64_t ImportKlineRaw(TAOS* conn, VipdocReader& reader,
                         Market market, const std::string& code,
                         const char* subdir, const char* ext,
                         const char* period) {
    std::vector<KLine> bars;
    if (std::strcmp(ext, "day") == 0)
      bars = reader.ReadDay(market, code);
    else if (std::strcmp(ext, "lc1") == 0)
      bars = reader.ReadMin1(market, code);
    else if (std::strcmp(ext, "lc5") == 0)
      bars = reader.ReadMin5(market, code);
    if (bars.empty()) return 0;

    std::string tb = "k_" + code + "_" + period;

    // 自动判断增量/全量：查子表最新 ts
    int64_t last_ts = LastTimestamp(conn, tb);
    std::vector<KLine> new_bars;
    if (last_ts > 0) {
      for (auto& b : bars)
        if (b.datetime * 1000LL > last_ts) new_bars.push_back(std::move(b));
    } else {
      new_bars = std::move(bars);
    }
    if (new_bars.empty()) return 0;

    std::string esc_code = EscapeSql(code);
    std::string esc_period = EscapeSql(period);

    int64_t written = 0;
    for (size_t i = 0; i < new_bars.size(); i += kBatchSize) {
      size_t end = std::min(i + kBatchSize, new_bars.size());
      std::ostringstream sql;
      sql << "INSERT INTO " << tb << " USING kline TAGS('"
          << esc_code << "','" << esc_period << "') VALUES";
      for (size_t j = i; j < end; ++j) {
        if (j > i) sql << ' ';
        const auto& b = new_bars[j];
        char row[256];
        std::snprintf(row, sizeof(row),
          "(%lld,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f)",
          (long long)(b.datetime * 1000LL),
          b.open, b.high, b.low, b.close, b.volume, b.amount);
        sql << row;
      }
      int64_t n = ExecAffected(conn, sql.str());
      if (n < 0) return written;
      written += static_cast<int64_t>(end - i);
    }
    return written;
  }
};

}  // namespace

namespace tdx::taos {

ImportResult DoImportTaos(const ImportTaosConfig& cfg) {
  ImportResult result;

  // 1. vipdoc 根目录
  std::string tdx_home = cfg.vipdoc_path;
  if (tdx_home.size() > 7 && tdx_home.substr(tdx_home.size() - 7) == "/vipdoc")
    tdx_home = tdx_home.substr(0, tdx_home.size() - 7);

  // 2. 扫描代码
  std::vector<std::pair<Market, std::string>> targets;
  if (!cfg.codes.empty()) {
    for (const auto& c : cfg.codes)
      targets.emplace_back(MarketFromCode(c), c);
  } else {
    targets = ScanCodes(tdx_home);
  }
  if (targets.empty()) {
    std::cerr << "未发现可导入的代码（vipdoc=" << cfg.vipdoc_path << "）\n";
    return result;
  }
  result.codes_total = static_cast<int>(targets.size());

  // 3. 连接 TDengine + 建库建表
  // 先不选库连 server（db=tdx 不存在时 taos_connect(db="tdx") 会失败）
  {
    TaosConfig init_cfg = cfg.taos;
    init_cfg.db.clear();
    TaosConnection conn(init_cfg);
    if (!conn) {
      std::cerr << "TDengine 连接失败，无法导入。\n";
      return result;
    }
    ExecSQL(conn.native(),
            "CREATE DATABASE IF NOT EXISTS tdx "
            "KEEP 36500 DURATION 50 REPLICA 1");
  }
  TaosConnection conn(cfg.taos);
  if (!conn) {
    std::cerr << "TDengine 连接 tdx 库失败。\n";
    return result;
  }
  ExecSQL(conn.native(), "USE tdx");
  ExecSQL(conn.native(),
          "CREATE STABLE IF NOT EXISTS kline ("
          "ts TIMESTAMP, open DOUBLE, high DOUBLE, low DOUBLE, "
          "close DOUBLE, volume DOUBLE, amount DOUBLE) "
          "TAGS (code VARCHAR(10), cycle VARCHAR(8))");
  ExecSQL(conn.native(),
          "CREATE STABLE IF NOT EXISTS adjust ("
          "ts TIMESTAMP, fenhong DOUBLE, peigujia DOUBLE, songzhuangu DOUBLE, "
          "peigu DOUBLE, category INT, name VARCHAR(64)) "
          "TAGS (code VARCHAR(10))");

  // 4. 多线程导入
  int n_threads = cfg.jobs > 0 ? cfg.jobs
                               : static_cast<int>(std::thread::hardware_concurrency());
  if (n_threads < 1) n_threads = 1;
  if (n_threads > static_cast<int>(targets.size()))
    n_threads = static_cast<int>(targets.size());

  // 构建 worker config（vipdoc_path 替换为 tdx_home，避免 vipdoc/vipdoc 双重路径）
  ImportTaosConfig worker_cfg = cfg;
  worker_cfg.vipdoc_path = tdx_home;

  std::cout << "引擎:     TDengine " << cfg.taos.host << ":" << cfg.taos.port
            << "/" << cfg.taos.db << "\n"
            << "vipdoc:   " << tdx_home << "\n"
            << "股票数:   " << targets.size() << "\n"
            << "线程数:   " << n_threads << "\n"
            << "复权:     " << (cfg.no_adjust ? "跳过" : "开启") << "\n"
            << "模式:     自动（查 MAX(ts) 增量/全量）\n"
            << "写入:     batch INSERT (1000行/批)\n\n";

  std::vector<std::thread> threads;
  std::vector<Worker> workers;
  workers.reserve(n_threads);

  for (int w = 0; w < n_threads; ++w) {
    Worker worker{w, worker_cfg, {}};
    size_t total = targets.size();
    size_t start = total * w / n_threads;
    size_t end = total * (w + 1) / n_threads;
    worker.subset.assign(targets.begin() + start, targets.begin() + end);
    workers.push_back(std::move(worker));
  }

  auto t0 = std::chrono::steady_clock::now();
  for (auto& w : workers)
    threads.emplace_back(&Worker::Run, &w);
  for (auto& t : threads) t.join();

  // 5. 汇总
  for (const auto& w : workers) {
    result.codes_ok += w.codes_ok;
    result.kline_rows += w.kline_rows;
    result.adjust_events += w.adjust_events;
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();

  std::cout << "\n=== 导入完成 ===\n"
            << "股票:   " << result.codes_ok << "/" << result.codes_total << "\n"
            << "K线:    " << result.kline_rows << " 行\n"
            << "复权:   " << result.adjust_events << " 条\n"
            << "耗时:   " << elapsed << "s\n"
            << "数据库: tdx @ " << cfg.taos.host << ":" << cfg.taos.port << "\n";

  return result;
}

}  // namespace tdx::taos
