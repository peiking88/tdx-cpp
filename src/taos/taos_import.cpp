// TDengine 多线程导入实现。
// 架构：
//   Step 0) 建库建表
//   Step 1) SyncStockNames — 网络拉取全量码表入 stock_name
//   Step 2) KlineWorker × N 线程：过滤 + 本地文件导入 + 标记缺失（无网络=无 ProactorPool 冲突）
//   Step 3) BatchNetImport：单 ProactorPool + N fiber → 缺失代码 K线(1d/1m/5m) + 全量复权
//   Step 4) CleanupStaleCodes
// ponytail: 用 raw SQL INSERT 替代 STMT——STMT bind_param_batch 在 helio/absl
// 进程内崩溃（libtaos.so internal memcpy SIGSEGV，独立测试通过），暂不可用。
#include "tdx/taos/taos_import.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/asio/ip/address.hpp>

#include "util/fibers/fibers.h"
#include "util/fibers/synchronization.h"

#include "tdx/consts.hpp"
#include "tdx/proto/connection.hpp"
#include "tdx/proto/frame.hpp"
#include "tdx/proto/parsers.hpp"
#include "tdx/proto/server_pool.hpp"
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

// 前置声明（IsAStock 定义在文件末尾）
namespace tdx::taos { bool IsAStock(const std::string& code); }

namespace {

using tdx::taos::IsAStock;

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

// 批量 INSERT K 线数据（共享工具，消除重复）。
// 返回写入行数；中途失败返回 -1（已写入数据保留在 DB 中，下次增量会跳过）。
static int64_t InsertKlineBatch(TAOS* conn, const std::string& tb,
                                 const std::string& code, const std::string& cycle,
                                 const std::vector<KLine>& bars) {
  std::string esc = EscapeSql(code);
  int64_t written = 0;
  for (size_t i = 0; i < bars.size(); i += kBatchSize) {
    size_t end = std::min(i + kBatchSize, bars.size());
    std::ostringstream sql;
    sql << "INSERT INTO " << tb << " USING kline TAGS('" << esc << "','" << cycle << "') VALUES";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      const auto& b = bars[j];
      char row[256];
      std::snprintf(row, sizeof(row),
        "(%lld,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f)",
        (long long)(b.datetime * 1000LL), b.open, b.high, b.low, b.close, b.volume, b.amount);
      sql << row;
    }
    int64_t n = ExecAffected(conn, sql.str());
    if (n < 0) return -1;  // 中断，已写入数据保留（下次增量跳过）
    written += static_cast<int64_t>(end - i);
  }
  return written;
}

// ---- K线 Worker：过滤检查 + 本地文件导入 + 标记缺失代码 ----
struct KlineWorker {
  int id;
  const ImportTaosConfig& cfg;
  std::vector<std::pair<std::string, std::string>> subset;
  std::vector<std::string> missing;       // 本地无 .day 文件的代码
  std::vector<std::string> filtered_out;  // 被 IsAStock 过滤的代码

  int codes_ok = 0;
  int64_t kline_rows = 0;

  void Run() {
    TaosConnection conn(cfg.taos);
    if (!conn) return;
    ExecSQL(conn.native(), "USE tdx");

    VipdocReader reader(cfg.vipdoc_path);

    for (const auto& [code, name] : subset) {
      Market market = tdx::MarketFromCode(code);

      if (!tdx::taos::IsAStock(code)) {
        ExecAffected(conn.native(), "DROP TABLE IF EXISTS k_" + code + "_1d");
        ExecAffected(conn.native(), "DROP TABLE IF EXISTS k_" + code + "_1m");
        ExecAffected(conn.native(), "DROP TABLE IF EXISTS k_" + code + "_5m");
        ExecAffected(conn.native(), "DROP TABLE IF EXISTS a_" + code);
        ExecAffected(conn.native(),
                     "DELETE FROM stock_name WHERE code='" + EscapeSql(code) + "'");
        filtered_out.push_back(code);
        continue;
      }

      std::string mdir = VipdocReader::MarketDir(market);
      std::string fpath = cfg.vipdoc_path + "/vipdoc/" + mdir + "/lday/"
                        + mdir + code + ".day";

      if (fs::exists(fpath)) {
        // 三个周期独立导入，任一周期的 DB 错误不影响其他周期
        bool ok_any = false;
        auto imp = [&](const char* sd, const char* ext, const char* per) {
          int64_t r = ImportKlineRaw(conn.native(), reader, market, code, sd, ext, per);
          if (r < 0) return;      // DB 写入错误（已写部分保留）
          if (r > 0) { kline_rows += r; ok_any = true; }
        };
        imp("lday", "day", "1d");
        imp("minline", "lc1", "1m");
        imp("fzline", "lc5", "5m");
        if (ok_any) ++codes_ok;
      } else {
        missing.push_back(code);  // 标记，后续批量网络拉取
      }
    }
  }

 private:
  int64_t ImportKlineRaw(TAOS* conn, VipdocReader& reader,
                         Market market, const std::string& code,
                         const char* subdir, const char* ext, const char* period) {
    std::vector<KLine> bars;
    if (std::strcmp(ext, "day") == 0)
      bars = reader.ReadDay(market, code);
    else if (std::strcmp(ext, "lc1") == 0)
      bars = reader.ReadMin1(market, code);
    else if (std::strcmp(ext, "lc5") == 0)
      bars = reader.ReadMin5(market, code);
    if (bars.empty()) return 0;

    std::string tb = "k_" + code + "_" + period;
    int64_t last_ts = LastTimestamp(conn, tb);
    std::vector<KLine> new_bars;
    if (last_ts > 0) {
      for (auto& b : bars)
        if (b.datetime * 1000LL > last_ts) new_bars.push_back(std::move(b));
    } else {
      new_bars = std::move(bars);
    }
    if (new_bars.empty()) return 0;

    return InsertKlineBatch(conn, tb, code, period, new_bars);
  }
};

// 从 DB 读取调整因子回退（网络失败时使用）
static std::vector<tdx::Xdxr> ReadAdjustFromDB(TAOS* conn, const std::string& code) {
  std::vector<tdx::Xdxr> xdxr;
  std::string sql = "SELECT ts, fenhong, peigujia, songzhuangu, peigu, category, name FROM a_" + code;
  TAOS_RES* res = ::taos_query(conn, sql.c_str());
  if (res && ::taos_errno(res) == 0) {
    TAOS_ROW row;
    while ((row = ::taos_fetch_row(res))) {
      tdx::Xdxr x;
      if (row[0]) {
        int64_t ts_ms = *static_cast<const int64_t*>(row[0]);
        auto ct = tdx::util::epoch_to_cst(ts_ms / 1000);
        char dbuf[32];
        std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", ct.year, ct.month, ct.day);
        x.date = dbuf;
      }
      if (row[1]) x.fenhong = *static_cast<const double*>(row[1]);
      if (row[2]) x.peigujia = *static_cast<const double*>(row[2]);
      if (row[3]) x.songzhuangu = *static_cast<const double*>(row[3]);
      if (row[4]) x.peigu = *static_cast<const double*>(row[4]);
      if (row[5]) x.category = *static_cast<const int*>(row[5]);
      if (row[6]) {
        const auto* p = static_cast<const unsigned char*>(row[6]);
        uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
        x.name = std::string(reinterpret_cast<const char*>(p), len);
      }
      xdxr.push_back(std::move(x));
    }
  }
  ::taos_free_result(res);
  return xdxr;
}

// ---- 批量网络拉取：单 ProactorPool + N fiber（避免 StdQuotes 多池冲突） ----
// 对每只 code：若在 missing_set 中则拉 1d/1m/5m K线 + 复权，否则只拉复权。
struct NetImportResult { int kline_ok = 0; int64_t kline_rows = 0; int adj_ok = 0; int adj_events = 0; };
static NetImportResult BatchNetImport(
    const ImportTaosConfig& cfg,
    const std::set<std::string>& missing,          // 需拉 K 线的代码
    const std::vector<std::string>& all_codes,     // 所有代码（至少需拉复权）
    int n_fibers,
    bool no_adjust) {
  NetImportResult r;
  if (all_codes.empty()) return r;
  if (n_fibers < 1) n_fibers = 1;

  using mutex_type = ::util::fb2::Mutex;
  mutex_type mu;

  std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
  pool->Run();
  auto* pb = pool->GetNextProactor();

  proto::ServerPool sp(pool.get());
  auto best = sp.SelectBest(quotes::StdQuotes::DefaultHosts());
  if (!best) { pool->Stop(); return r; }

  auto addr = boost::asio::ip::make_address(best->ip);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  pb->Await([&] {
    std::vector<::util::fb2::Fiber> workers;
    workers.reserve(n_fibers);
    for (int wi = 0; wi < n_fibers; ++wi) {
      workers.push_back(::util::MakeFiber([&, wi] {
        TaosConnection tconn(cfg.taos);
        if (!tconn) return;
        ExecSQL(tconn.native(), "USE tdx");

        proto::Connection conn(pb);
        if (conn.Connect(ep)) return;
        try {
          auto login = proto::serialize_login();
          auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                         login.data(), login.size());
          conn.Call(req);
        } catch (...) {
          std::fprintf(stderr, "BatchNetImport: fiber %d 登录失败，跳过\n", wi);
          return;
        }

        int loc_kl = 0, loc_adj = 0;
        int64_t loc_rows = 0;

        // 拉取单周期 K 线并增量入库
        auto PullKline = [&](const std::string& code, Market market, Period period, const char* tag) {
          std::string tb = "k_" + code + "_" + tag;
          int64_t last_ts = LastTimestamp(tconn.native(), tb);
          std::vector<KLine> new_bars;
          for (uint16_t off = 0; off < kKlineMaxCount; off += 800) {
            auto body = proto::serialize_kline(market, code, period, 1, off, 800, Adjust::NONE);
            auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgKline,
                                   body.data(), body.size()));
            auto bars = proto::deserialize_kline(resp.body.data(), resp.body.size(), period);
            if (bars.empty()) break;
            for (auto& b : bars) {
              if (last_ts <= 0 || b.datetime * 1000LL > last_ts) new_bars.push_back(std::move(b));
            }
            if (bars.size() < 800) break;
          }
          if (new_bars.empty()) return false;

          int64_t n = InsertKlineBatch(tconn.native(), tb, code, tag, new_bars);
          if (n < 0) return false;  // DB 写入错误，已写部分保留
          loc_rows += n;
          return true;
        };

        // 拉取复权因子并增量入库
        auto PullAdjust = [&](const std::string& code, Market market) {
          std::string tb = "a_" + code;
          int64_t last_ts = LastTimestamp(tconn.native(), tb);

          std::vector<tdx::Xdxr> xdxr;
          try {
            auto body = proto::serialize_xdxr(market, code);
            auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgXdxr,
                                   body.data(), body.size()));
            xdxr = proto::deserialize_xdxr(resp.body.data(), resp.body.size());
          } catch (const std::exception&) {
            // 回退到库中已有数据，不中止
          }

          if (xdxr.empty())
            xdxr = ReadAdjustFromDB(tconn.native(), code);
          if (xdxr.empty()) return;

          int n = 0;
          for (const auto& x : xdxr) {
            int64_t ts = tdx::util::date_to_epoch(
                std::stoi(x.date.substr(0, 4)), std::stoi(x.date.substr(5, 2)),
                std::stoi(x.date.substr(8, 2))) * 1000LL;
            if (last_ts > 0 && ts <= last_ts) continue;
            char sql[1024];
            std::snprintf(sql, sizeof(sql),
              "INSERT INTO %s USING adjust TAGS('%s') "
              "VALUES(%lld, %.4f, %.4f, %.4f, %.4f, %d, '%s')",
              tb.c_str(), code.c_str(), (long long)ts,
              x.fenhong, x.peigujia, x.songzhuangu, x.peigu,
              x.category, EscapeSql(x.name).c_str());
            if (ExecAffected(tconn.native(), sql) >= 0) ++n;
          }
          if (n > 0) { loc_adj += n; }
        };

        for (size_t i = static_cast<size_t>(wi); i < all_codes.size();
             i += static_cast<size_t>(n_fibers)) {
          const auto& code = all_codes[i];
          auto market = tdx::MarketFromCode(code);

          if (missing.count(code)) {
            bool ok_any = false;
            if (PullKline(code, market, Period::DAILY, "1d")) ok_any = true;
            if (PullKline(code, market, Period::MIN_1, "1m")) ok_any = true;
            if (PullKline(code, market, Period::MIN_5, "5m")) ok_any = true;
            if (ok_any) ++loc_kl;  // per-code, not per-period
          }
          if (!no_adjust) PullAdjust(code, market);
        }
        conn.Close();

        std::lock_guard<mutex_type> lk(mu);
        r.kline_ok += loc_kl;
        r.kline_rows += loc_rows;
        r.adj_ok += (loc_adj > 0 ? 1 : 0);
        r.adj_events += loc_adj;
      }));
    }
    for (auto& w : workers) w.Join();
  });

  pool->Stop();
  return r;
}

}  // namespace

namespace tdx::taos {

// 清理非 A 股及退市标的：遍历 kline/adjust 子表，删除不在 stock_name 中的代码。
int CleanupStaleCodes(TAOS* conn) {
  std::cerr << "清理:     扫描过期标的..." << std::flush;

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
  if (valid.empty()) { std::cerr << " stock_name 为空，跳过" << std::endl; return 0; }

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
      // 仅清理不通过 IsAStock 的标的（债券/B股/港股通），保留服务器未返回的合法 A 股（如 589xxx ETF）
      if (!valid.count(code) && !IsAStock(code)) stale.insert(code);
    }
  }
  ::taos_free_result(res);

  if (stale.empty()) { std::cerr << " 无需清理" << std::endl; return 0; }

  int dropped = 0;
  for (const auto& code : stale) {
    for (auto* period : periods) {
      std::string tb = "k_" + code + "_" + period;
      if (ExecAffected(conn, "DROP TABLE IF EXISTS " + tb) >= 0) ++dropped;
    }
    if (ExecAffected(conn, "DROP TABLE IF EXISTS a_" + code) >= 0) ++dropped;
  }
  std::cerr << " " << stale.size() << " 只代码 / " << dropped << " 张子表" << std::endl;
  return static_cast<int>(stale.size());
}

// 全量拉取沪深京 A 股名称写入 TDengine stock_name 表。
// 先写入临时表 stock_name_tmp → 网络成功则原子替换，失败则清临时表保留旧 stock_name。
int SyncStockNames(TAOS* conn) {
  std::cerr << "股票名称: 同步中..." << std::flush;

  ExecSQL(conn, "DROP TABLE IF EXISTS stock_name_tmp");
  ExecSQL(conn,
          "CREATE TABLE stock_name_tmp ("
          "ts TIMESTAMP, code VARCHAR(10), name VARCHAR(64))");

  quotes::StdQuotes sq;
  if (auto ec = sq.Connect(); ec) {
    std::cerr << " StdQuotes 连接失败（" << ec.message() << "）\n";
    ExecSQL(conn, "DROP TABLE IF EXISTS stock_name_tmp");
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
      sql << "INSERT INTO stock_name_tmp VALUES";
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

  // 原子替换：重建空表 → 拷贝 → 成功才删临时表
  ExecSQL(conn, "DROP TABLE IF EXISTS stock_name");
  ExecSQL(conn, "CREATE TABLE stock_name (ts TIMESTAMP, code VARCHAR(10), name VARCHAR(64))");
  if (ExecSQL(conn, "INSERT INTO stock_name SELECT * FROM stock_name_tmp")) {
    ExecSQL(conn, "DROP TABLE IF EXISTS stock_name_tmp");
  } else {
    std::cerr << "【警告】码表拷贝失败！stock_name_tmp 保留（" << count
              << " 条），可手动恢复或重试。" << std::endl;
  }

  std::cerr << " " << count << " 条" << std::endl;
  return count;
}

ImportResult DoImportTaos(const ImportTaosConfig& cfg) {
  ImportResult result;

  // 1. vipdoc 根目录
  std::string tdx_home = cfg.vipdoc_path;
  if (tdx_home.size() > 7 && tdx_home.substr(tdx_home.size() - 7) == "/vipdoc")
    tdx_home = tdx_home.substr(0, tdx_home.size() - 7);

  // 2. 连接 TDengine + 建库建表
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

  // === 第一步：网络拉取全量码表入库 ===
  std::cerr << "=== 第一步：同步全量码表 ===" << std::endl;
  result.stock_names = SyncStockNames(conn.native());
  if (result.stock_names == 0) {
    // 离线回退：用库中已有 stock_name 继续
    TAOS_RES* chk = ::taos_query(conn.native(),
        "SELECT count(*) FROM stock_name");
    bool has_old = chk && ::taos_errno(chk) == 0;
    if (has_old) {
      TAOS_ROW row = ::taos_fetch_row(chk);
      if (row && row[0])
        result.stock_names = static_cast<int>(*static_cast<int64_t*>(row[0]));
    }
    ::taos_free_result(chk);
    if (result.stock_names == 0) {
      std::cerr << "码表同步失败且库中无旧码表，无法继续。\n";
      return result;
    }
    std::cerr << "网络失败，回退到库中已有码表 (" << result.stock_names << " 条)" << std::endl;
  }

  // === 第二步：读取码表 + 多线程本地导入 + 标记缺失 ===
  std::vector<std::pair<std::string, std::string>> all_codes;
  {
    TAOS_RES* res = ::taos_query(conn.native(), "SELECT code, name FROM stock_name");
    if (res && ::taos_errno(res) == 0) {
      TAOS_ROW row;
      while ((row = ::taos_fetch_row(res))) {
        std::string code, name;
        if (row[0]) {
          const auto* p = static_cast<const unsigned char*>(row[0]);
          uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
          code = std::string(reinterpret_cast<const char*>(p), len);
        }
        if (row[1]) {
          const auto* p = static_cast<const unsigned char*>(row[1]);
          uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
          name = std::string(reinterpret_cast<const char*>(p), len);
        }
        if (!code.empty()) all_codes.emplace_back(std::move(code), std::move(name));
      }
    }
    ::taos_free_result(res);
  }
  result.codes_total = static_cast<int>(all_codes.size());
  if (all_codes.empty()) { std::cerr << "码表为空，无代码可处理。\n"; return result; }

  if (!cfg.codes.empty()) {
    std::set<std::string> want(cfg.codes.begin(), cfg.codes.end());
    std::vector<std::pair<std::string, std::string>> t;
    for (auto& p : all_codes) if (want.count(p.first)) t.push_back(std::move(p));
    if (t.empty()) { std::cerr << "指定代码均不在码表中。\n"; return result; }
    all_codes = std::move(t);
    result.codes_total = static_cast<int>(all_codes.size());
  }

  int n_threads = cfg.jobs > 0 ? cfg.jobs
                               : static_cast<int>(std::thread::hardware_concurrency());
  if (n_threads < 1) n_threads = 1;
  if (n_threads > static_cast<int>(all_codes.size()))
    n_threads = static_cast<int>(all_codes.size());

  ImportTaosConfig worker_cfg = cfg;
  worker_cfg.vipdoc_path = tdx_home;

  std::cerr << "引擎:     TDengine " << cfg.taos.host << ":" << cfg.taos.port
            << "/" << cfg.taos.db << "\n"
            << "vipdoc:   " << tdx_home << "\n"
            << "代码数:   " << result.codes_total << "\n"
            << "线程数:   " << n_threads << "\n"
            << "写入:     batch INSERT (1000行/批)\n" << std::endl;

  auto t0 = std::chrono::steady_clock::now();

  // ---- 步骤 A：本地文件导入（多线程，无网络 → 无 ProactorPool 冲突） ----
  std::vector<std::string> missing_codes;
  std::set<std::string> filtered_set;
  {
    std::cerr << "=== 第二步：本地 K线导入（" << n_threads << " 线程）===" << std::endl;

    std::vector<std::thread> threads;
    std::vector<KlineWorker> workers;
    workers.reserve(n_threads);

    for (int w = 0; w < n_threads; ++w) {
      KlineWorker worker{w, worker_cfg};
      size_t start = all_codes.size() * w / n_threads;
      size_t end = all_codes.size() * (w + 1) / n_threads;
      worker.subset.assign(all_codes.begin() + start, all_codes.begin() + end);
      workers.push_back(std::move(worker));
    }

    for (auto& w : workers)
      threads.emplace_back(&KlineWorker::Run, &w);
    for (auto& t : threads) t.join();

    int filtered = 0;
    for (auto& w : workers) {
      result.codes_ok += w.codes_ok;
      result.kline_rows += w.kline_rows;
      filtered += static_cast<int>(w.filtered_out.size());
      for (auto& c : w.filtered_out) filtered_set.insert(std::move(c));
      for (auto& c : w.missing) missing_codes.push_back(std::move(c));
    }

    std::cerr << "K线: " << result.kline_rows << " 行 / " << result.codes_ok
              << " 只有效, 缺失: " << missing_codes.size()
              << ", 过滤: " << filtered << std::endl;

    // 移除被过滤代码
    if (!filtered_set.empty()) {
      all_codes.erase(std::remove_if(all_codes.begin(), all_codes.end(),
          [&](auto& p) { return filtered_set.count(p.first); }), all_codes.end());
    }
  }

  // ---- 步骤 B：网络拉取（单 ProactorPool + N fiber：K线 + 复权合并） ----
  {
    std::set<std::string> missing_set(missing_codes.begin(), missing_codes.end());
    std::vector<std::string> adjust_codes;
    for (const auto& p : all_codes) adjust_codes.push_back(p.first);

    int n_fibers = std::min(n_threads, static_cast<int>(adjust_codes.size()));
    if (n_fibers < 1) n_fibers = 1;

    std::cerr << "=== 第三步：网络拉取（" << adjust_codes.size()
              << " 只 / " << n_fibers << " fiber";
    if (!missing_codes.empty())
      std::cerr << ", K线缺 " << missing_codes.size() << " 只";
    std::cerr << "）===" << std::endl;

    auto nr = BatchNetImport(worker_cfg, missing_set, adjust_codes, n_fibers, cfg.no_adjust);
    result.codes_ok += nr.kline_ok;
    result.kline_rows += nr.kline_rows;
    result.adjust_events += nr.adj_events;

    std::cerr << "K线: " << nr.kline_rows << " 行 / " << nr.kline_ok
              << " 只, 复权: " << nr.adj_events << " 条 / " << nr.adj_ok
              << " 只" << std::endl;
  }

  // ---- 步骤 C：清理 ----
  int cleaned = CleanupStaleCodes(conn.native());

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - t0).count();

  std::cerr << "\n=== 导入完成 ===\n"
            << "股票:   " << result.codes_ok << "/" << result.codes_total << "\n"
            << "K线:    " << result.kline_rows << " 行\n"
            << "复权:   " << result.adjust_events << " 条\n"
            << "名称:   " << result.stock_names << " 条\n"
            << "清理:   " << cleaned << " 只\n"
            << "耗时:   " << elapsed << "s\n"
            << "数据库: tdx @ " << cfg.taos.host << ":" << cfg.taos.port << std::endl;

  return result;
}

// 从网络拉取全部周期 K线写入 TDengine（ponytail: 单 StdQuotes 串行）。
NetworkImportResult ImportKlineFromNetwork(TAOS* conn,
                                           const std::vector<std::string>& codes) {
  NetworkImportResult result;
  if (codes.empty()) return result;

  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) {
    std::cerr << "ImportKlineFromNetwork: 连接服务器失败 " << ec.message() << "\n";
    return result;
  }

  for (const auto& code : codes) {
    auto market = MarketFromCode(code);
    bool ok_any = false;

    // 拉取单周期网络 K 线
    auto pull = [&](Period p, const char* tag) {
      std::string tb = "k_" + code + "_" + tag;
      int64_t last_ts = LastTimestamp(conn, tb);
      std::vector<KLine> new_bars;
      for (uint16_t off = 0; off < kKlineMaxCount; off += 800) {
        auto bars = sq.Bars(market, code, p, off, 800);
        if (bars.empty()) break;
        for (auto& b : bars) {
          if (last_ts <= 0 || b.datetime * 1000LL > last_ts) new_bars.push_back(std::move(b));
        }
        if (bars.size() < 800) break;
      }
      if (new_bars.empty()) return;

      int64_t n = InsertKlineBatch(conn, tb, code, tag, new_bars);
      if (n > 0) { result.kline_rows += n; ok_any = true; }
    };

    pull(Period::DAILY, "1d");
    pull(Period::MIN_1, "1m");
    pull(Period::MIN_5, "5m");
    if (ok_any) ++result.codes_ok;
  }
  sq.Close();
  return result;
}

// 保留 A 股 + 指数 + ETF + 基金，排除债券(1x/2x)、B 股(9x)、港股通(7x)
bool IsAStock(const std::string& code) {
  if (code.size() < 6) return false;
  char c0 = code[0];
  char c1 = code[1];
  if (c0 == '0' || c0 == '3') return true;
  if (c0 == '6') return true;
  if (c0 == '4') return true;
  if (c0 == '8' && c1 != '8') return true;
  if (c0 == '8' && c1 == '8') return true;
  if (c0 == '5') return true;
  if (c0 == '1') {
    if (c1 == '5' && code[2] == '9') return true;
    if (c1 == '6') return true;
  }
  if (c0 == '9' && c1 == '9') return true;   // 99xxxx 沪市指数（999999 上证指数等），排除 900xxx B股
  return false;
}

}  // namespace tdx::taos
