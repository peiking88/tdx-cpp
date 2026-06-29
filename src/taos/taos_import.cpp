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
#include <set>
#include <unordered_map>
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

// 复权缓存：代码 → 除权除息事件列表
using XdxrCache = std::unordered_map<std::string, std::vector<tdx::Xdxr>>;

// 单 StdQuotes 预取全部标的复权因子，避免多 worker 各创 ProactorPool(64 IO 线程)
// 引发 helio fiber scheduler 冲突（"Fibers belong to different schedulers"）。
// 返回 code → Xdxr 映射；网络失败容错（打印警告并继续）。
static XdxrCache PrefetchXdxr(
    const std::vector<std::pair<Market, std::string>>& targets) {
  XdxrCache cache;
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect(); ec) {
    std::fprintf(stderr, "复权预取: StdQuotes 连接失败 (%s)，跳过复权\n",
                 ec.message().c_str());
    return cache;
  }

  int ok = 0, fail = 0;
  for (const auto& [market, code] : targets) {
    try {
      auto xdxr = sq.GetXdxr(market, code);
      if (!xdxr.empty()) {
        cache[code] = std::move(xdxr);
        ++ok;
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "复权预取: %s 失败 %s\n", code.c_str(), e.what());
      ++fail;
    }
  }
  sq.Close();
  std::cout << "复权预取: " << ok << " 只有效 / " << fail << " 失败\n";
  return cache;
}

namespace {

// 保留 A 股 + 指数 + ETF + 基金，排除债券(1x/2x)、B 股(9x)、港股通(7x)
static bool IsAStock(const std::string& code) {
  if (code.size() < 6) return false;
  char c0 = code[0];
  char c1 = code[1];
  // A 股
  if (c0 == '0' || c0 == '3') return true;   // 深市主板/中小 000-003 / 创业板 300-301 / 深证指数 399
  if (c0 == '6') return true;                 // 沪市主板 600-605 / 科创板 688
  // 北交所 A 股
  if (c0 == '4') return true;                 // 4xxxxx
  if (c0 == '8' && c1 != '8') return true;    // 8xxxxx（非板块指数）
  // 指数
  if (c0 == '8' && c1 == '8') return true;    // 88xxxx 板块指数
  // ETF / LOF 基金
  if (c0 == '5') return true;                 // 沪市 ETF/LOF (5xxxxx)
  if (c0 == '1') {
    if (c1 == '5' && code[2] == '9') return true;  // 深市 ETF (159xxx)
    if (c1 == '6') return true;                    // 深市 LOF (16xxxx)
  }
  // 排除: 1xxxxx(债券,除 159/16), 2xxxxx(B 股/债券), 7xxxxx(港股通), 9xxxxx(B 股)
  return false;
}

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
      if (!IsAStock(code)) continue;  // 跳过债券/B 股/港股通
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
  const XdxrCache& xdxr_cache;  // 主线程预取的复权因子（只读，多线程安全）

  int codes_ok = 0;
  int64_t kline_rows = 0;
  int adjust_events = 0;

  void Run() {
    TaosConnection conn(cfg.taos);
    if (!conn) return;

    // USE tdx
    ExecSQL(conn.native(), "USE tdx");

    VipdocReader reader(cfg.vipdoc_path);

    for (const auto& [market, code] : subset) {
      // === K 线 ===
      int64_t n = ImportKlineRaw(conn.native(), reader, market, code, "lday", "day", "1d");
      n += ImportKlineRaw(conn.native(), reader, market, code, "minline", "lc1", "1m");
      n += ImportKlineRaw(conn.native(), reader, market, code, "fzline", "lc5", "5m");
      if (n > 0) { kline_rows += n; ++codes_ok; }

      // === 复权（从预取缓存读取，无网络调用）===
      auto it = xdxr_cache.find(code);
      if (it == xdxr_cache.end() || it->second.empty()) continue;

      const auto& xdxr = it->second;
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

// 清理非 A 股及退市标的：遍历 kline/adjust 子表，删除不在 stock_name 中的代码。
int CleanupStaleCodes(TAOS* conn) {
  std::cout << "清理:     扫描过期标的..." << std::flush;

  // 收集 stock_name 中的有效代码
  std::set<std::string> valid;
  TAOS_RES* res = ::taos_query(conn, "SELECT code FROM stock_name");
  if (res && ::taos_errno(res) == 0) {
    TAOS_ROW row;
    while ((row = ::taos_fetch_row(res))) {
      if (row[0]) {
        const auto* p = static_cast<const unsigned char*>(row[0]);
        uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
        valid.insert(std::string(reinterpret_cast<const char*>(p), len));
      }
    }
  }
  ::taos_free_result(res);
  if (valid.empty()) { std::cout << " stock_name 为空，跳过\n"; return 0; }

  // 扫描 kline 子表
  std::set<std::string> stale;
  const char* periods[] = {"1d", "1m", "5m"};
  res = ::taos_query(conn, "SELECT DISTINCT code FROM kline");
  if (res && ::taos_errno(res) == 0) {
    TAOS_ROW row;
    while ((row = ::taos_fetch_row(res))) {
      if (!row[0]) continue;
      const auto* p = static_cast<const unsigned char*>(row[0]);
      uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
      std::string code(reinterpret_cast<const char*>(p), len);
      if (!valid.count(code)) stale.insert(code);
    }
  }
  ::taos_free_result(res);

  if (stale.empty()) { std::cout << " 无需清理\n"; return 0; }

  int dropped = 0;
  for (const auto& code : stale) {
    for (auto* period : periods) {
      std::string tb = "k_" + code + "_" + period;
      if (ExecAffected(conn, "DROP TABLE IF EXISTS " + tb) >= 0) ++dropped;
    }
    if (ExecAffected(conn, "DROP TABLE IF EXISTS a_" + code) >= 0) ++dropped;
  }
  std::cout << " " << stale.size() << " 只代码 / " << dropped << " 张子表\n";
  return static_cast<int>(stale.size());
}

// 全量拉取沪深京 A 股名称写入 TDengine stock_name 表。
// ponytail: DROP+CREATE+批量INSERT，保证一致性。
int SyncStockNames(TAOS* conn) {
  std::cout << "股票名称: 同步中..." << std::flush;

  ExecSQL(conn, "DROP TABLE IF EXISTS stock_name");
  ExecSQL(conn,
          "CREATE TABLE stock_name ("
          "ts TIMESTAMP, code VARCHAR(10), name VARCHAR(64))");

  quotes::StdQuotes sq;
  if (auto ec = sq.Connect(); ec) {
    std::cerr << " StdQuotes 连接失败（" << ec.message() << "）\n";
    return 0;
  }

  int count = 0;
  int64_t ts_seq = 0;
  for (auto market : {Market::SH, Market::SZ, Market::BJ}) {
    uint16_t total = sq.StockCount(market);
    for (uint16_t start = 0; start < total; start += 1600) {
      auto stocks = sq.Stocks(market, start, 1600);
      if (stocks.empty()) continue;

      std::ostringstream sql;
      sql << "INSERT INTO stock_name VALUES";
      size_t batch_cnt = 0;
      for (size_t i = 0; i < stocks.size(); ++i) {
        if (!IsAStock(stocks[i].code)) continue;
        if (batch_cnt > 0) sql << ' ';
        sql << "(" << ts_seq++ << ", '" << EscapeSql(stocks[i].code) << "', '"
            << EscapeSql(stocks[i].name) << "')";
        ++batch_cnt;
      }
      if (batch_cnt == 0) continue;
      ExecAffected(conn, sql.str());
      count += static_cast<int>(batch_cnt);
    }
  }
  sq.Close();
  std::cout << " " << count << " 条\n";
  return count;
}

ImportResult DoImportTaos(const ImportTaosConfig& cfg) {
  ImportResult result;

  // 1. vipdoc 根目录
  std::string tdx_home = cfg.vipdoc_path;
  if (tdx_home.size() > 7 && tdx_home.substr(tdx_home.size() - 7) == "/vipdoc")
    tdx_home = tdx_home.substr(0, tdx_home.size() - 7);

  // 2. 扫描代码
  std::vector<std::pair<Market, std::string>> targets;
  if (!cfg.codes.empty()) {
    for (const auto& c : cfg.codes) {
      if (!IsAStock(c)) {
        std::cerr << "跳过非 A 股代码: " << c << "\n";
        continue;
      }
      targets.emplace_back(MarketFromCode(c), c);
    }
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

  // 4. 预取复权因子（单 StdQuotes，避免多 worker 各创 ProactorPool 引发
  //    helio "Fibers belong to different schedulers" 告警）
  XdxrCache xdxr_cache;
  if (!cfg.no_adjust) {
    xdxr_cache = PrefetchXdxr(targets);
  }

  // 5. 多线程导入
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
    Worker worker{w, worker_cfg, {}, xdxr_cache};
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

  // 6. 汇总
  for (const auto& w : workers) {
    result.codes_ok += w.codes_ok;
    result.kline_rows += w.kline_rows;
    result.adjust_events += w.adjust_events;
  }

  // 7. 同步股票代码→名称对照表
  result.stock_names = SyncStockNames(conn.native());

  // 8. 清理非 A 股及退市标的子表
  int cleaned = CleanupStaleCodes(conn.native());

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();

  std::cout << "\n=== 导入完成 ===\n"
            << "股票:   " << result.codes_ok << "/" << result.codes_total << "\n"
            << "K线:    " << result.kline_rows << " 行\n"
            << "复权:   " << result.adjust_events << " 条\n"
            << "名称:   " << result.stock_names << " 条\n"
            << "清理:   " << cleaned << " 只\n"
            << "耗时:   " << elapsed << "s\n"
            << "数据库: tdx @ " << cfg.taos.host << ":" << cfg.taos.port << "\n";

  return result;
}

}  // namespace tdx::taos
