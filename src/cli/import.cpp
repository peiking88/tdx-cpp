// tdx import：TDX 本地历史数据 → DuckDB 导入工具（单线程）。
//   1. 读取本地 .lc1/.lc5/.day → DuckDB 表（1m/5m/1d）
//   2. 自动重采样 15m/30m/60m/week/mon
//   3. 网络获取复权因子，持久化 + 增量更新
//   4. 全量导入 / 每日增量导入（import_state 表追踪进度）
//
// 用法：
//   tdx import [full] [codes...]
//   环境变量:
//     TDX_HOME         vipdoc 路径
//     TDX_IMPORT_DB    DuckDB 数据库路径
//     TDX_NO_ADJUST=1  跳过复权因子
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/data/resampler.hpp"
#include "tdx/proto/vipdoc_reader.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/query/duckdb_query.hpp"
#include "tdx/taos/taos_import.hpp"
#include "tdx/types.hpp"
#include "tdx/util/time_util.hpp"

namespace fs = std::filesystem;
using namespace tdx;

namespace {

struct ImportConfig {
  std::string vipdoc_path = "/home/li/.local/share/tdxcfv/drive_c/tc/vipdoc";
  std::string db_path = "output/tdx_kline.db";
  std::string engine = "duckdb";  // "duckdb" | "taos"
  bool full = false;
  bool no_adjust = false;
  std::vector<std::string> codes;
  tdx::taos::TaosConfig taos = tdx::taos::TaosConfig::FromEnv();
};

ImportConfig ParseArgs(int argc, char** argv) {
  ImportConfig cfg;
  const char* home = std::getenv("TDX_HOME");
  if (home) cfg.vipdoc_path = home;
  const char* db = std::getenv("TDX_IMPORT_DB");
  if (db) cfg.db_path = db;
  if (std::getenv("TDX_NO_ADJUST")) cfg.no_adjust = true;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    // 跳过 absl flag（absl 不自动从 argv 移除）
    if (a == "--jobs") { ++i; continue; }
    if (a.rfind("--jobs=", 0) == 0) continue;
    if (a == "full") cfg.full = true;
    else if (a == "taos") cfg.engine = "taos";
    else if (a == "duckdb") cfg.engine = "duckdb";
    else if (a == "help") {
      std::cout << "用法: tdx import [taos|duckdb] [full] [codes...]\n\n"
                << "  taos|duckdb      存储引擎（默认 duckdb）\n"
                << "  full             全量导入（默认增量）\n"
                << "  codes...         股票代码（默认扫描 vipdoc 全部）\n\n"
                << "环境变量:\n"
                << "  TDX_HOME         vipdoc 路径（当前: " << cfg.vipdoc_path << "）\n"
                << "  TDX_IMPORT_DB    DuckDB 路径（当前: " << cfg.db_path << "）\n"
                << "  TDX_NO_ADJUST=1  跳过复权因子\n"
                << "  TDX_TAOS_HOST    TDengine 主机（默认 localhost）\n"
                << "  TDX_TAOS_PORT    TDengine 端口（默认 6030）\n"
                << "  TDX_TAOS_USER    TDengine 用户（默认 root）\n"
                << "  TDX_TAOS_PASS    TDengine 密码（默认 taosdata）\n"
                << "  TDX_TAOS_DB      数据库名（默认 tdx）\n\n"
                << "示例:\n"
                << "  tdx import                      增量导入（DuckDB）\n"
                << "  tdx import taos full            全量导入（TDengine）\n"
                << "  tdx import taos 600000          导入单只股票\n"
                << "  tdx import taos 600000 -n 4     4 线程并发导入\n";
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

// 增量同步的 last_datetime 缓存：key = code|period
using LastDtMap = std::unordered_map<std::string, int64_t>;

// 读文件 + 重采样 + 写入 DB（单线程）。
// full=false（增量）时：主表（1d/1m/5m）只写 datetime > last_dt 的新 K 线；
// 派生表（周/月/5m/15m/30m/60m）用完整原始序列重采样后全量 INSERT OR REPLACE（覆盖）。
// 返回写入的 K 线条数。
int64_t ReadResampleWrite(tdx::proto::VipdocReader& reader,
                           tdx::query::DuckDBQuery& db,
                           const std::string& code, Market market, bool full,
                           const LastDtMap& last_dt) {
  int64_t written = 0;

  auto MaxDt = [](const std::vector<KLine>& bars) -> int64_t {
    int64_t md = 0;
    for (const auto& b : bars) md = std::max(md, b.datetime);
    return md;
  };

  auto WriteState = [&](const std::string& period, int64_t dt) {
    char s[384];
    std::snprintf(s, sizeof(s), "('%s', '%s', %lld)", code.c_str(), period.c_str(),
                  static_cast<long long>(dt));
    db.Exec("INSERT OR REPLACE INTO import_state VALUES " + std::string(s));
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
      db.InsertKlines("kline_1d", full ? bars_1d : new_1d, code, full);
      written += (full ? bars_1d : new_1d).size();
      db.InsertKlines("kline_1w", tdx::data::ResampleKline(bars_1d, tdx::data::Freq::Weekly), code, false);
      db.InsertKlines("kline_1mon", tdx::data::ResampleKline(bars_1d, tdx::data::Freq::Monthly), code, false);
      WriteState("1d", MaxDt(bars_1d));
    }
  }

  // ---- 1 分钟线 ----
  auto bars_1m = reader.ReadMin1(market, code);
  if (!bars_1m.empty()) {
    auto new_1m = FilterNew(bars_1m, "1m");
    if (full || !new_1m.empty()) {
      db.InsertKlines("kline_1m", full ? bars_1m : new_1m, code, full);
      written += (full ? bars_1m : new_1m).size();
      db.InsertKlines("kline_5m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min5), code, false);
      db.InsertKlines("kline_15m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min15), code, false);
      db.InsertKlines("kline_30m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Min30), code, false);
      db.InsertKlines("kline_60m", tdx::data::ResampleKline(bars_1m, tdx::data::Freq::Hour1), code, false);
      WriteState("1m", MaxDt(bars_1m));
    }
  } else {
    // 5 分钟线（原生，仅在 1m 不可用时）
    auto bars_5m = reader.ReadMin5(market, code);
    if (!bars_5m.empty()) {
      auto new_5m = FilterNew(bars_5m, "5m");
      if (full || !new_5m.empty()) {
        db.InsertKlines("kline_5m", full ? bars_5m : new_5m, code, full);
        written += (full ? bars_5m : new_5m).size();
        db.InsertKlines("kline_15m", tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Min15), code, false);
        db.InsertKlines("kline_30m", tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Min30), code, false);
        db.InsertKlines("kline_60m", tdx::data::ResampleKline(bars_5m, tdx::data::Freq::Hour1), code, false);
        WriteState("5m", MaxDt(bars_5m));
      }
    }
  }

  return written;
}

}  // namespace

int DoImport(int argc, char** argv, int jobs) {
  auto cfg = ParseArgs(argc, argv);

  // ---- TDengine 引擎路由 ----
  if (cfg.engine == "taos") {
    tdx::taos::ImportTaosConfig tcfg;
    tcfg.taos       = cfg.taos;
    tcfg.vipdoc_path = cfg.vipdoc_path;
    tcfg.full        = cfg.full;
    tcfg.no_adjust   = cfg.no_adjust;
    tcfg.jobs        = jobs;
    tcfg.codes       = cfg.codes;
    auto result = tdx::taos::DoImportTaos(tcfg);
    return (result.codes_ok > 0) ? 0 : 1;
  }

  // ---- DuckDB 引擎（原有逻辑）----
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
            << "模式:   " << (cfg.full ? "全量导入" : "增量导入") << "\n\n";

  // 2. 确保 output 目录存在
  std::error_code mkdir_ec;
  fs::create_directories("output", mkdir_ec);

  // 3. 初始化数据库
  tdx::query::DuckDBQuery db(cfg.db_path);
  db.ConfigureForImport();
  {
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

  // 增量模式：预载 import_state
  LastDtMap last_dt_map;
  if (!cfg.full) last_dt_map = db.LoadImportState();

  // === 单线程本地导入 ===
  std::cout << "=== 本地数据导入 ===\n";
  tdx::proto::VipdocReader reader(tdx_home);
  int64_t total_bars = 0;
  int code_count = 0;
  auto t0 = std::chrono::steady_clock::now();

  for (int i = 0; i < n_total; ++i) {
    const auto& [market, code] = targets[static_cast<size_t>(i)];
    int64_t n = ReadResampleWrite(reader, db, code, market, cfg.full, last_dt_map);
    if (n > 0) {
      total_bars += n;
      code_count++;
    }

    // 进度（每 10 只或最后一只）
    if ((i + 1) % 10 == 0 || i == n_total - 1) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - t0).count();
      int pct = (i + 1) * 100 / n_total;
      std::cout << "\r[" << pct << "%] " << (i + 1) << "/" << n_total
                << "  已导入 " << total_bars << " 根K线  " << elapsed << "s" << std::flush;
    }
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cout << "\r[100%] " << n_total << "/" << n_total
            << "  本地导入: " << total_bars << " 根 K 线（"
            << code_count << " 只股票有新数据）  "
            << elapsed << "s\n";

  // === 单线程复权因子 ===
  if (!cfg.no_adjust) {
    std::cout << "\n=== 复权因子（单线程 + 50ms 限流） ===\n";
    std::unordered_set<std::string> existing;
    if (!cfg.full) existing = db.LoadAdjustCodes();

    quotes::StdQuotes sq;
    if (auto ec = sq.Connect()) {
      std::cerr << "复权连接失败: " << ec.message() << "\n";
    } else {
      int adj_events = 0;
      int consecutive_errors = 0;
      auto t1 = std::chrono::steady_clock::now();

      for (int i = 0; i < n_total; ++i) {
        const auto& [market, code] = targets[static_cast<size_t>(i)];

        if (!cfg.full && existing.count(code)) {
          consecutive_errors = 0;
          goto progress;
        }

        try {
          auto xdxr_list = sq.GetXdxr(market, code);
          consecutive_errors = 0;
          if (!xdxr_list.empty()) {
            db.Exec("BEGIN TRANSACTION");
            int inserted = 0;
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
            adj_events += inserted;
          }
        } catch (const std::exception&) {
          if (++consecutive_errors >= 3) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            consecutive_errors = 0;
          }
        }

      progress:
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 进度（每 10 只或最后一只）
        if ((i + 1) % 10 == 0 || i == n_total - 1) {
          auto adj_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - t1).count();
          int pct = (i + 1) * 100 / n_total;
          std::cout << "\r[" << pct << "%] " << (i + 1) << "/" << n_total
                    << "  复权事件 " << adj_events << " 条  " << adj_elapsed << "s" << std::flush;
        }
      }
      sq.Close();

      auto adj_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - t1).count();
      std::cout << "\r[100%] " << n_total << "/" << n_total
                << "  复权事件: " << adj_events << " 条  " << adj_elapsed << "s\n";
    }
  } else {
    std::cout << "\n跳过复权因子。\n";
  }

  std::cout << "\n全部完成。数据库: " << cfg.db_path << "\n";
  return 0;
}
