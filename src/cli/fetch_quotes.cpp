// tdx fetch-quotes：实时行情采集 + TDengine 写入。支持单次 / 循环模式。
// 支持多种数据类型：quote(0x53e) / transaction(0xfc5) / tick(0x537) / index_info(0x51d) / unusual(0x563)
//
// 用法：
//   tdx fetch-quotes [--loop] [--interval 30] [--jobs 8]
//                    [--codes 600000,000001]
//                    [--with-tx] [--with-tick] [--with-index] [--with-unusual]
//
// 架构：N fiber worker 分片 → 独立 Connection → 各类型 network call → TaosConnection → INSERT
// 对齐 batch_fetch.cpp 并发模式。
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <csignal>
#include <ctime>
#include <thread>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "base/init.h"
#include "util/fibers/fibers.h"
#include "util/fibers/pool.h"
#include "util/fibers/synchronization.h"

#include "absl/flags/flag.h"

#include "tdx/consts.hpp"
#include "tdx/proto/connection.hpp"
#include "tdx/proto/frame.hpp"
#include "tdx/proto/parsers.hpp"
#include "tdx/util/gbk.hpp"
#include "tdx/util/code_validate.hpp"
#include "tdx/proto/server_pool.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/quotes/ext_quotes.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/taos/taos_import.hpp"
#include "tdx/types.hpp"
#include "tdx/shm/payload.hpp"
#include "tdx/shm/segment.hpp"

#include "base/ProducerConsumerQueue.h"  // folly::ProducerConsumerQueue（SPSC lock-free，任意线程可用）

#include <taos.h>

ABSL_FLAG(bool, quote_hk, false, "同时采集港股（按代码分桶走扩展行情端口 7727，协议 0x248a）");
ABSL_FLAG(std::string, quote_codes, "", "股票代码列表（逗号分隔），不指定则拉取全市场");
ABSL_FLAG(int32_t, quote_jobs, 8, "并发 worker 数");
ABSL_FLAG(int32_t, quote_batch, 80, "单次网络请求股票数（≤200）");
ABSL_FLAG(bool, quote_loop, false, "循环采集模式");
ABSL_FLAG(int32_t, quote_interval, 30, "循环间隔（秒）");
ABSL_FLAG(bool, with_tx, false, "同步采集逐笔成交 0xfc5");
ABSL_FLAG(bool, with_tick, false, "同步采集分时图 0x537");
ABSL_FLAG(bool, with_index, false, "同步采集指数信息 0x51d");
ABSL_FLAG(bool, with_unusual, false, "同步采集主力异动 0x563");
ABSL_FLAG(bool, with_vol, false, "采集成交量分布 0x51a");
ABSL_FLAG(bool, with_hist, false, "采集历史委托+逐笔 0xfb4/0xfb5");
ABSL_FLAG(bool, with_board, false, "采集板块列表+资金流向 0x1231/0x1218");
ABSL_FLAG(bool, all_market, false, "采集全市场（默认仅采集自选股 zxg.blk）");
ABSL_FLAG(std::string, zxg_blk,
          "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk",
          "通达信自选股文件（每行7位：首位1=沪/0=深+6位代码）。可用环境变量 TDX_ZXG_BLK 覆盖");
ABSL_FLAG(std::string, mmap_path, "",
          "盘中实时行情共享内存文件路径（建议 /dev/shm/xxx）；"
          "非空则启用「写 mmap 快照 + 异步入库」，空则退回同步入库（向后兼容）");

using namespace tdx;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

// Worker 分桶：A 股（标准行情，msg_id 0x53e）/ 港股（扩展行情，msg_id 0x248a）。
// 同服务器的一批 code 走同一协议（A 股 0x53e / 港股 0x248a 请求格式互不兼容），不同桶分别选服、
// 分别建连接。
enum class WorkerMarket : uint8_t { AShare, HK };

std::vector<std::string> SplitCodes(const std::string& s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::string cur;
  for (char ch : s) {
    if (ch == ',') { if (!cur.empty()) out.push_back(std::move(cur)); cur.clear(); }
    else cur += ch;
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// 盘中实时行情筛选：只保留指数、个股、ETF基金，排除 LOF/债券/B股/港股通
// code 可为 6 位纯数字（FetchAllCodes 来自服务器）或带 sh/sz/bj 前缀（v0.13.8 规范，
// --quote_codes 用户输入）。剥前缀后统一按 6 位判断。
bool IsQuoteTarget(std::string_view code) {
  if (code.size() >= 8) {
    auto pre = code.substr(0, 2);
    if (pre == "sh" || pre == "sz" || pre == "bj") code.remove_prefix(2);
  }
  if (code.size() < 2) return false;
  char c0 = code[0], c1 = code[1];
  if (c0 == '0' || c0 == '3') return true;             // A 股深市 + 399xxx 指数
  if (c0 == '6') return true;                           // A 股沪市
  if (c0 == '4') return true;                           // 北交所
  if (c0 == '8') return c1 == '8';                      // 88xxxx 板块指数（去掉 8xxxxx 北交所）
  if (c0 == '9' && c1 == '9') return true;              // 99xxxx 沪市指数
  if (c0 == '5') return true;                           // 5xxxxx 沪市 ETF
  if (c0 == '1') return c1 == '5' && code.size() > 2
                       && code[2] == '9';                 // 159xxx 深市 ETF（排除 16xxxx LOF）
  return false;
}

std::vector<std::string> FetchAllCodes() {
  std::vector<std::string> all;
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return all; }
  for (auto market : {Market::SH, Market::SZ, Market::BJ}) {
    uint16_t total = sq.StockCount(market);
    for (uint16_t start = 0; start < total; start += 1600) {
      auto stocks = sq.Stocks(market, start, std::min<uint16_t>(1600, total - start));
      for (auto& s : stocks) if (IsQuoteTarget(s.code)) all.push_back(s.code);
    }
  }
  sq.Close();
  return all;
}

// 读取通达信自选股 zxg.blk：纯文本 CRLF，每行 7 位（首位 1=沪 sh / 0=深 sz + 6 位代码）。
std::vector<std::string> ReadZxgBlk(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  if (!f) { std::cerr << "无法读取自选股文件: " << path << "\n"; return out; }
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (line.size() < 7) continue;          // 非代码行（空行/注释）跳过
    char m = line[0];
    if (m != '0' && m != '1') continue;     // 首位须为市场标志（1=沪/0=深）
    out.push_back((m == '1' ? "sh" : "sz") + line.substr(1, 6));
  }
  return out;
}

std::string Esc(std::string_view s) {
  std::string out; out.reserve(s.size() + 4);
  for (char ch : s) { if (ch == '\'') out += "''"; else out += ch; }
  return out;
}

// ---- 建表 & INSERT 辅助 ----

void EnsureTables(TAOS* conn, bool do_tx, bool do_tick, bool do_idx, bool do_unu,
                  bool do_vol, bool do_hist) {
  tdx::taos::ExecSQL(conn, "USE tdx");
  tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS quote (ts TIMESTAMP, price DOUBLE, pre_close DOUBLE, "
    "open DOUBLE, high DOUBLE, low DOUBLE, volume DOUBLE, amount DOUBLE, "
    "bid1 DOUBLE, bid2 DOUBLE, bid3 DOUBLE, bid4 DOUBLE, bid5 DOUBLE, "
    "ask1 DOUBLE, ask2 DOUBLE, ask3 DOUBLE, ask4 DOUBLE, ask5 DOUBLE, "
    "bid_vol1 DOUBLE, bid_vol2 DOUBLE, bid_vol3 DOUBLE, bid_vol4 DOUBLE, bid_vol5 DOUBLE, "
    "ask_vol1 DOUBLE, ask_vol2 DOUBLE, ask_vol3 DOUBLE, ask_vol4 DOUBLE, ask_vol5 DOUBLE) "
    "TAGS (code VARCHAR(10))");
  if (do_tx) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS trans (ts TIMESTAMP, price DOUBLE, volume BIGINT, "
    "trans_id BIGINT, buy_sell TINYINT) TAGS (code VARCHAR(10))");
  if (do_tick) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS tick (ts TIMESTAMP, price DOUBLE, avg DOUBLE, volume DOUBLE) "
    "TAGS (code VARCHAR(10))");
  if (do_idx) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS idx_info (ts TIMESTAMP, close DOUBLE, pre_close DOUBLE, "
    "diff DOUBLE, open DOUBLE, high DOUBLE, low DOUBLE, volume DOUBLE, amount DOUBLE, "
    "up_count BIGINT, down_count BIGINT) TAGS (code VARCHAR(10))");
  if (do_unu) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS unusual (ts TIMESTAMP, unusual_type TINYINT, "
    "descr VARCHAR(64), val DOUBLE, val_str VARCHAR(64)) TAGS (code VARCHAR(10))");
  if (do_vol) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS vol (ts TIMESTAMP, price DOUBLE, pre_close DOUBLE, "
    "open DOUBLE, high DOUBLE, low DOUBLE, volume DOUBLE, amount DOUBLE) "
    "TAGS (code VARCHAR(10))");
  if (do_hist) {
    tdx::taos::ExecSQL(conn,
      "CREATE STABLE IF NOT EXISTS hist_ord (ts TIMESTAMP, price DOUBLE, vol BIGINT) "
      "TAGS (code VARCHAR(10))");
    tdx::taos::ExecSQL(conn,
      "CREATE STABLE IF NOT EXISTS hist_tx2 (ts TIMESTAMP, price DOUBLE, vol BIGINT, "
      "buy_sell TINYINT) TAGS (code VARCHAR(10))");
  }
}

int64_t ExecSql(TAOS* conn, const std::string& sql) {
  TAOS_RES* res = ::taos_query(conn, sql.c_str());
  int code = ::taos_errno(res);
  if (code != 0) {
    // 打印错误码 + SQL 前缀（方便定位问题股票和时间戳）
    std::cerr << "DB err[" << code << "] " << (res ? ::taos_errstr(res) : "NULL-res") << "\n";
    if (sql.size() <= 500) std::cerr << "  SQL: " << sql << "\n";
    else std::cerr << "  SQL[0:500]: " << sql.substr(0, 500) << "...\n";
    ::taos_free_result(res);
    return -1;
  }
  int64_t n = ::taos_affected_rows(res);
  ::taos_free_result(res);
  return n;
}

int64_t InsertQuote(TAOS* conn, const std::vector<Quote>& quotes, int64_t now_ms) {
  if (quotes.empty()) return 0;
  auto V = [](double v) -> std::string {
    if (std::isnan(v)) return "NULL";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
  };
  int64_t written = 0;
  for (size_t i = 0; i < quotes.size(); i += 200) {
    size_t end = std::min(i + 200, quotes.size());
    std::ostringstream sql; sql << "INSERT INTO ";
    for (size_t j = i; j < end; ++j) {
      if (!tdx::util::IsValidCode(quotes[j].code)) continue;
      if (j > i) sql << ' ';
      sql << "q_" << quotes[j].code << " USING quote TAGS('" << Esc(quotes[j].code) << "')";
      const auto& q = quotes[j];
      int64_t ts = q.datetime > 0 ? q.datetime * 1000LL : now_ms;
      if (ts > 4102444800000LL) ts = now_ms;  // 防御 to_datetime 垃圾 year 溢出
      sql << " VALUES(" << ts << "," << q.price << "," << q.pre_close << "," << q.open
          << "," << q.high << "," << q.low << "," << q.volume << "," << q.amount;
      for (int k = 0; k < 5; ++k) sql << "," << V(q.bid[k]);
      for (int k = 0; k < 5; ++k) sql << "," << V(q.ask[k]);
      for (int k = 0; k < 5; ++k) sql << "," << V(q.bid_vol[k]);
      for (int k = 0; k < 5; ++k) sql << "," << V(q.ask_vol[k]);
      sql << ")";
    }
    int64_t n = ExecSql(conn, sql.str());
    if (n >= 0) { written += static_cast<int64_t>(end - i); continue; }
    // 批量 INSERT 失败 → 逐条重试，定位问题股票且不影响其余数据入库
    for (size_t j = i; j < end; ++j) {
      if (!tdx::util::IsValidCode(quotes[j].code)) continue;
      std::ostringstream one;
      const auto& q = quotes[j];
      int64_t ts = q.datetime > 0 ? q.datetime * 1000LL : now_ms;
      if (ts > 4102444800000LL) ts = now_ms;  // 防御 to_datetime 垃圾 year 溢出
      one << "INSERT INTO q_" << q.code << " USING quote TAGS('" << Esc(q.code) << "') VALUES("
          << ts << "," << q.price << "," << q.pre_close << "," << q.open
          << "," << q.high << "," << q.low << "," << q.volume << "," << q.amount;
      for (int k = 0; k < 5; ++k) one << "," << V(q.bid[k]);
      for (int k = 0; k < 5; ++k) one << "," << V(q.ask[k]);
      for (int k = 0; k < 5; ++k) one << "," << V(q.bid_vol[k]);
      for (int k = 0; k < 5; ++k) one << "," << V(q.ask_vol[k]);
      one << ")";
      int64_t r = ExecSql(conn, one.str());
      if (r >= 0) ++written;
      // 失败时 ExecSql 已打印 SQL（含时间戳和代码），继续下一条
    }
  }
  return written;
}

int64_t InsertTx(TAOS* conn, const std::vector<Transaction>& txns,
                  std::string_view code, int64_t day_ts) {
  if (txns.empty() || !tdx::util::IsValidCode(code)) return 0;
  std::string esc = Esc(std::string(code)); int64_t written = 0;
  for (size_t i = 0; i < txns.size(); i += 500) {
    size_t end = std::min(i + 500, txns.size());
    std::ostringstream sql;
    sql << "INSERT INTO tx_" << code << " USING trans TAGS('" << esc << "') VALUES";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      const auto& t = txns[j];
      sql << "(" << (day_ts + t.datetime * 1000LL) << "," << t.price << "," << t.volume
          << "," << t.trans_id << "," << static_cast<int>(t.buy_sell) << ")";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

int64_t InsertTick(TAOS* conn, const std::vector<Tick>& ticks,
                    std::string_view code, int64_t day_ts) {
  if (ticks.empty() || !tdx::util::IsValidCode(code)) return 0;
  std::string esc = Esc(std::string(code)); int64_t written = 0;
  for (size_t i = 0; i < ticks.size(); i += 500) {
    size_t end = std::min(i + 500, ticks.size());
    std::ostringstream sql;
    sql << "INSERT INTO tk_" << code << " USING tick TAGS('" << esc << "') VALUES";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      const auto& t = ticks[j];
      sql << "(" << (day_ts + t.datetime * 1000LL) << "," << t.price << "," << t.avg << "," << t.volume << ")";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

int64_t InsertIdx(TAOS* conn, const IndexInfo& ii, int64_t now_ms) {
  if (!tdx::util::IsValidCode(ii.code)) return 0;
  std::ostringstream sql;
  sql << "INSERT INTO ix_" << ii.code << " USING idx_info TAGS('" << Esc(ii.code) << "') VALUES("
      << now_ms << "," << ii.close << "," << ii.pre_close << "," << ii.diff
      << "," << ii.open << "," << ii.high << "," << ii.low << "," << ii.vol << "," << ii.amount
      << "," << ii.up_count << "," << ii.down_count << ")";
  return ExecSql(conn, sql.str());
}

int64_t InsertUnu(TAOS* conn, const std::vector<UnusualItem>& items, int64_t now_ms) {
  if (items.empty()) return 0; int64_t written = 0;
  for (size_t i = 0; i < items.size(); i += 100) {
    size_t end = std::min(i + 100, items.size());
    std::ostringstream sql; sql << "INSERT INTO ";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      const auto& u = items[j];
      sql << "un_" << u.code << " USING unusual TAGS('" << Esc(u.code) << "') VALUES("
          << now_ms << "," << u.unusual_type << ",'" << Esc(u.desc) << "',"
          << u.value << ",'" << Esc(u.value_str) << "')";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

// 判断是否为指数代码（仅指数收集 index_info 和 unusual）。
// market 参数区分 00xxxx 沪市=指数 vs 深市=个股（v0.14.4）。
bool IsIndexCode(std::string_view code, Market market) {
  if (code.size() < 2) return false;
  char c0 = code[0], c1 = code[1];
  if (c0 == '0' && c1 == '0' && market == Market::SH) return true;  // 000xxx 上证指数
  return (c0 == '8' && c1 == '8') || (c0 == '9' && c1 == '9') || (c0 == '3' && c1 == '9')
         || (code.size() > 2 && c0 == '3' && c1 == '9' && code[2] == '9');
}

// 获取当日 0 点 epoch ms（用于把逐笔的分钟偏移转换为绝对时间）
int64_t DayStartMs() {
  auto t = std::chrono::system_clock::now();
  time_t now = std::chrono::system_clock::to_time_t(t);
  struct tm lt{};
  localtime_r(&now, &lt);
  lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
  return static_cast<int64_t>(mktime(&lt)) * 1000LL;
}

// ==================== 采集核心 ====================
// ---- vol/hist INSERT helpers（f10/finance 已分离到独立命令 tdx f10/finance）----
int64_t InsertVol(TAOS* conn, const VolProfile& vp, int64_t now_ms) {
  if (!tdx::util::IsValidCode(vp.code)) return 0;
  std::ostringstream sql;
  sql << "INSERT INTO vl_" << vp.code << " USING vol TAGS('" << Esc(vp.code) << "') VALUES("
      << now_ms << "," << vp.price << "," << vp.pre_close << "," << vp.open
      << "," << vp.high << "," << vp.low << "," << vp.vol << "," << vp.amount << ")";
  return ExecSql(conn, sql.str());
}

int64_t InsertHistOrd(TAOS* conn, const std::vector<HistoryOrder>& orders,
                       std::string_view code, int64_t base_ts) {
  if (orders.empty() || !tdx::util::IsValidCode(code)) return 0;
  std::string esc = Esc(std::string(code)); int64_t written = 0;
  for (size_t i = 0; i < orders.size(); i += 500) {
    size_t end = std::min(i + 500, orders.size());
    std::ostringstream sql;
    sql << "INSERT INTO ho_" << code << " USING hist_ord TAGS('" << esc << "') VALUES";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      sql << "(" << base_ts << "," << orders[j].price << "," << orders[j].vol << ")";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

int64_t InsertHistTx2(TAOS* conn, const std::vector<HistoryTransaction>& txns,
                       std::string_view code, int64_t base_ts) {
  if (txns.empty() || !tdx::util::IsValidCode(code)) return 0;
  std::string esc = Esc(std::string(code)); int64_t written = 0;
  for (size_t i = 0; i < txns.size(); i += 500) {
    size_t end = std::min(i + 500, txns.size());
    std::ostringstream sql;
    sql << "INSERT INTO ht_" << code << " USING hist_tx2 TAGS('" << esc << "') VALUES";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      int h = txns[j].minutes / 60 % 24, m = txns[j].minutes % 60;
      sql << "(" << (base_ts + (h * 3600 + m * 60) * 1000LL) << ","
          << txns[j].price << "," << txns[j].vol << "," << txns[j].buy_sell << ")";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

// 两阶段架构（对齐 taos_import.cpp:256-257）：
//   阶段 A（fiber）：只做网络 I/O + 解析，数据存入 FetchChunk
//   阶段 B（主线程）：TaosConnection 写入 TDengine
// 严禁在 fiber 内调用 taos_* —— TDengine 内部阻塞 + 可能与 helio Proactor 冲突致堆损坏。

// fiber 阶段收集的单批数据
struct FetchChunk {
  std::vector<Quote> quotes;
  struct TxData { std::string code; std::vector<Transaction> txns; };
  struct TickData { std::string code; std::vector<Tick> ticks; };
  std::vector<TxData> txns;
  std::vector<TickData> ticks;
  std::vector<IndexInfo> indices;
  std::vector<UnusualItem> unusuals;
  std::vector<VolProfile> vols;
  struct HistOrdData { std::string code; std::vector<HistoryOrder> orders; };
  struct HistTxData { std::string code; std::vector<HistoryTransaction> txns; };
  std::vector<HistOrdData> hist_ords;
  std::vector<HistTxData> hist_txs;
  int errors = 0;
};

void WorkerRun(::util::fb2::ProactorBase* pb,
               ::util::FiberSocketBase::endpoint_type ep,
               WorkerMarket wm,
               const std::vector<std::string>& codes, int n, int batch_size,
               bool do_tx, bool do_tick, bool do_idx, bool do_unu,
               bool do_vol, bool do_hist,
               int wi, std::vector<FetchChunk>& chunks, ::util::fb2::Mutex& mu) {
  proto::Connection conn(pb);
  if (conn.Connect(ep)) { std::lock_guard<::util::fb2::Mutex> lk(mu); chunks.back().errors++; return; }
  try {
    if (wm == WorkerMarket::HK) {
      auto login = proto::serialize_ex_login();
      auto req = proto::pack_request(proto::kExHead, 0, proto::kMsgExLogin,
                                     login.data(), login.size());
      conn.Call(req);
    } else {
      auto login = proto::serialize_login();
      auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                     login.data(), login.size());
      conn.Call(req);
    }
  } catch (...) { std::lock_guard<::util::fb2::Mutex> lk(mu); chunks.back().errors++; return; }

  FetchChunk local;

  for (size_t bi = static_cast<size_t>(wi); bi < codes.size(); ) {
    if (g_stop) break;

    // ---- Quote (按 wm 选协议) ----
    std::vector<proto::QuoteReq> a_batch;  // A 股用 <B6s>
    std::vector<std::pair<ExMarket, std::string>> hk_batch;  // HK 用 <B23s>
    a_batch.reserve(batch_size);
    hk_batch.reserve(batch_size);
    for (size_t k = 0; k < static_cast<size_t>(batch_size); ++k) {
      size_t ci = bi + k * static_cast<size_t>(n);
      if (ci >= codes.size()) break;
      if (wm == WorkerMarket::HK) {
        hk_batch.push_back({ClassifyHkExplicit(codes[ci]), codes[ci]});
      } else {
        auto [mk, c] = ParseMarketCode(codes[ci]);
        if (c.empty()) { std::cerr << codes[ci] << ": 缺市场前缀，跳过\n"; continue; }
        a_batch.push_back({mk, c});
      }
    }
    bi += (wm == WorkerMarket::HK ? hk_batch.size() : a_batch.size()) * static_cast<size_t>(n);
    if (a_batch.empty() && hk_batch.empty()) continue;

    if (wm == WorkerMarket::HK && !hk_batch.empty()) {
      // HK: 0x248a 响应 unpack_futures 字段名不同（ExQuote），转 Quote 落库（OHLC 直接×1.0）。
      // HK 服务器不返回五档（bid/ask=0）。
      try {
        auto body = proto::serialize_ex_quotes(hk_batch);
        auto resp = conn.Call(proto::pack_request(proto::kExHead, 0, proto::kMsgExQuotes,
                                body.data(), body.size()));
        auto qs = proto::deserialize_ex_quotes(resp.body.data(), resp.body.size());
        for (auto& q : qs) {
          if (q.code.empty()) continue;
          Quote out;
          out.code = std::move(q.code);
          out.datetime = q.datetime;
          out.pre_close = q.pre_close;
          out.open = q.open;
          out.high = q.high;
          out.low = q.low;
          out.price = q.close;
          out.volume = q.vol;
          out.amount = q.amount;
          local.quotes.push_back(std::move(out));
        }
      } catch (...) { local.errors++; }
    } else if (!a_batch.empty()) {
      try {
        auto body = proto::serialize_quotes_detail(a_batch);
        auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgQuotesDetail,
                                body.data(), body.size()));
        auto qs = proto::deserialize_quotes_detail(resp.body.data(), resp.body.size());
        if (!qs.empty()) {
          local.quotes.insert(local.quotes.end(),
                              std::make_move_iterator(qs.begin()),
                              std::make_move_iterator(qs.end()));
        }
      } catch (...) { local.errors++; }
    }

    for (const auto& req : a_batch) {
      if (g_stop) break;
      auto mkt = req.market;
      const auto& code = req.code;

      if (do_tx) {
        try {
          auto body = proto::serialize_transaction(mkt, code, 0, 2000);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgTransaction,
                                  body.data(), body.size()));
          auto txns = proto::deserialize_transaction(resp.body.data(), resp.body.size());
          if (!txns.empty()) local.txns.push_back({std::string(code), std::move(txns)});
        } catch (...) {}
      }
      if (do_tick) {
        try {
          auto body = proto::serialize_tick(mkt, code, 0, kTickMaxCount);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgTick,
                                  body.data(), body.size()));
          auto ticks = proto::deserialize_tick(resp.body.data(), resp.body.size());
          if (!ticks.empty()) local.ticks.push_back({std::string(code), std::move(ticks)});
        } catch (...) {}
      }
      if (do_idx && IsIndexCode(code, mkt)) {
        try {
          auto body = proto::serialize_index_info(mkt, code);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgIndexInfo,
                                  body.data(), body.size()));
          auto ii = proto::deserialize_index_info(resp.body.data(), resp.body.size());
          if (!ii.code.empty()) local.indices.push_back(std::move(ii));
        } catch (...) {}
      }
      if (do_vol) {
        try {
          auto body = proto::serialize_volume_profile(mkt, code);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgVolumeProfile,
                                  body.data(), body.size()));
          auto vp = proto::deserialize_volume_profile(resp.body.data(), resp.body.size());
          if (!vp.code.empty()) local.vols.push_back(std::move(vp));
        } catch (...) {}
      }
    }

    // hist 是每股票数据：各 worker 采自己分片内的票（勿限 wi==0，否则只采 1/jobs）。
    // unusual 才是市场级（全局拉一次），仍限 wi==0（见下）。
    // f10/finance 已分离到独立命令 tdx f10 / tdx finance（清库重导），不在 fetch-quotes 采集。
    if (do_hist && wm == WorkerMarket::AShare) {
      time_t now_t = std::time(nullptr);
      struct tm now_tm{};
      localtime_r(&now_t, &now_tm);
      uint32_t today = static_cast<uint32_t>((now_tm.tm_year + 1900) * 10000 +
                      (now_tm.tm_mon + 1) * 100 + now_tm.tm_mday);
      for (const auto& req : a_batch) {
        try {
          auto body = proto::serialize_history_orders(req.market, req.code, today);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHistoryOrders,
                                  body.data(), body.size()));
          auto ord = proto::deserialize_history_orders(resp.body.data(), resp.body.size());
          if (!ord.empty()) local.hist_ords.push_back({std::string(req.code), std::move(ord)});
        } catch (...) {}
        try {
          auto body = proto::serialize_history_transaction(req.market, req.code, today, 0, 2000);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHistoryTransaction,
                                  body.data(), body.size()));
          auto txns = proto::deserialize_history_transaction(resp.body.data(), resp.body.size());
          if (!txns.empty()) local.hist_txs.push_back({std::string(req.code), std::move(txns)});
        } catch (...) {}
      }
    }
  }

  // unusual 是市场级数据（全局拉一次），移到分片循环外——原在 for(bi) 内随批次数重复拉取
  // （全市场场景 worker0 多批→重复 N 次网络+入库）。仅 A 股桶（HK 桶调用方置 do_unu=false）。
  if (do_unu && wi == 0 && wm == WorkerMarket::AShare) {
    try {
      auto body = proto::serialize_unusual(Market::SH, 0, 600);
      auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgUnusual,
                              body.data(), body.size()));
      auto items = proto::deserialize_unusual(resp.body.data(), resp.body.size());
      if (!items.empty()) {
        local.unusuals.insert(local.unusuals.end(),
                              std::make_move_iterator(items.begin()),
                              std::make_move_iterator(items.end()));
      }
    } catch (...) {}
  }

  conn.Close();
  std::lock_guard<::util::fb2::Mutex> lk(mu);
  chunks.push_back(std::move(local));
}

struct Counters {
  int64_t quote = 0, tx = 0, tick = 0, index = 0, unusual = 0;
  int64_t vol = 0, hist = 0;
  int errors = 0;
  int64_t dropped = 0;  // 异步入库队列满丢弃的 chunk 数（设计 §D4 背压）
  int64_t skipped = 0;  // 非当日数据被拦截数
};

// 过滤非法 Quote：datetime=0（服务器未返回时间，取当前时间）或 ±12h 内合法；
// 超出范围视为解析残留/脏数据拦截并记录。
bool IsQuoteValid(int64_t epoch_sec, int64_t now_epoch) {
  if (epoch_sec <= 0) return true;  // 0=服务器未返回时间，取当前时间
  int64_t delta = epoch_sec - now_epoch;
  return delta > -43200 && delta < 43200;
}

// 单 chunk 落库（同步路径与 IngestWorker 共用）。抽取自原阶段B 入库循环。
void IngestChunk(TAOS* conn, FetchChunk& ch, int64_t now_ms, int64_t day_ts,
                 bool do_tx, bool do_tick, bool do_idx, bool do_unu,
                 bool do_vol, bool do_hist, Counters& cnt) {
  if (!ch.quotes.empty()) {
    int64_t now_epoch = now_ms / 1000;
    std::vector<Quote> valid; valid.reserve(ch.quotes.size());
    std::set<std::string> skip_seen;  // 本 chunk 去重 [skip] 打印，避免 --loop 无限增长
    for (auto& q : ch.quotes) {
      if (!IsQuoteValid(q.datetime, now_epoch)) {
        cnt.skipped++;
        if (skip_seen.insert(q.code).second)
          std::cerr << "[skip] " << q.code << " ts=" << q.datetime << "\n";
        continue;
      }
      valid.push_back(std::move(q));
    }
    if (!valid.empty()) cnt.quote += InsertQuote(conn, valid, now_ms);
  }
  for (auto& td : ch.txns) cnt.tx += InsertTx(conn, td.txns, td.code, day_ts);
  for (auto& td : ch.ticks) cnt.tick += InsertTick(conn, td.ticks, td.code, day_ts);
  for (auto& ii : ch.indices) cnt.index += InsertIdx(conn, ii, now_ms);
  if (!ch.unusuals.empty()) cnt.unusual += InsertUnu(conn, ch.unusuals, now_ms);
  for (auto& vp : ch.vols) if (InsertVol(conn, vp, now_ms) > 0) cnt.vol++;
  for (auto& ho : ch.hist_ords) cnt.hist += InsertHistOrd(conn, ho.orders, ho.code, day_ts);
  for (auto& ht : ch.hist_txs) cnt.hist += InsertHistTx2(conn, ht.txns, ht.code, day_ts);
  cnt.errors += ch.errors;
}

// 单轮采集：两阶段——fiber 拉取 → 主线程写 mmap 快照 / 投递异步入库（或同步入库）
Counters RunOneRound(const std::vector<std::string>& codes, int jobs, int batch_size,
                     WorkerMarket wm,
                     bool do_tx, bool do_tick, bool do_idx, bool do_unu,
                     bool do_vol, bool do_hist,
                     tdx::shm::Segment* shm,
                     folly::ProducerConsumerQueue<FetchChunk>* ingest_q) {
  Counters cnt;
  if (codes.empty()) return cnt;

  // === 阶段 A：fiber 网络拉取（只用 proto::Connection，不触 TDengine） ===
  std::vector<FetchChunk> chunks;
  chunks.reserve(static_cast<size_t>(jobs));
  // placeholder chunk for error counting in WorkerRun
  chunks.emplace_back();

  {
    std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
    pool->Run();
    auto* pb = pool->GetNextProactor();

    proto::ServerPool sp(pool.get());
    auto hosts = (wm == WorkerMarket::HK) ? quotes::ExtQuotes::DefaultHosts()
                                          : quotes::StdQuotes::DefaultHosts();
    auto best = sp.SelectBest(std::move(hosts));
    if (!best) { std::cerr << "没有可用服务器\n"; pool->Stop(); cnt.errors++; return cnt; }
    std::cerr << (wm == WorkerMarket::HK ? "[HK] 选服: " : "选服: ")
              << best->name << " " << best->ip << ":" << best->port << "\n";

    auto addr = boost::asio::ip::make_address(best->ip);
    ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

    int n = std::min(jobs, static_cast<int>(codes.size()));
    if (n < 1) n = 1;

    ::util::fb2::Mutex mu;
    pb->Await([&] {
      std::vector<::util::fb2::Fiber> workers;
      workers.reserve(n);
      for (int wi = 0; wi < n; ++wi) {
        workers.push_back(::util::fb2::Fiber(
            ::util::fb2::Fiber::Opts{.stack_size = 128 * 1024},
            [&, wi] { WorkerRun(pb, ep, wm, codes, n, batch_size,
                                do_tx, do_tick, do_idx, do_unu,
                                do_vol, do_hist, wi, chunks, mu); }));
      }
      for (auto& w : workers) w.Join();
    });
    pool->Stop();
  }  // ProactorPool 析构，确保 helio 线程完全退出后再写 TDengine

  // === 阶段 B ===
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  int64_t day_ts = DayStartMs();

  if (shm && ingest_q) {
    // 异步路径：写 mmap 快照（对分析进程可见）+ 投递整轮 chunk 到 SPSC。
    // 主线程完全不碰 TDengine，立即进入下一轮 fetch（采集节拍与入库解耦，设计 §一/§D4）。
    std::set<std::string> skip_seen;  // 本轮去重 [skip] 打印，避免 --loop 无限增长
    for (auto& ch : chunks) {
      cnt.errors += ch.errors;
      for (auto& q : ch.quotes) {
        if (!IsQuoteValid(q.datetime, now_ms / 1000)) {
          cnt.skipped++;
          if (skip_seen.insert(q.code).second)
            std::cerr << "[skip] " << q.code << " ts=" << q.datetime << "\n";
          continue;
        }
        // ETF/基金（588xxx/510xxx/562xxx/15xxxx）服务器不返回时间戳，q.datetime == 0；
        // 与 InsertQuote 的入库逻辑一致：落盘 now_sec 作最新行情时间（否则 mmap 里存 epoch 0，
        // CST 下渲染成 08:00:00）。此时 IsQuoteValid(0)=true 已通过上方过滤。
        if (q.datetime <= 0) q.datetime = now_ms / 1000;
        shm->Snapshot().Put(q.code, tdx::shm::to_pod(q));
        cnt.quote++;  // 仅计实际写入 mmap 的有效 quote（与同步路径 InsertQuote 语义一致）
      }
      // 港股不入库（仅 mmap 实时快照供 viewer）：不投递入库队列，跳过 TDengine 写入。
      if (wm != WorkerMarket::HK && !ingest_q->write(std::move(ch))) {
        cnt.dropped++;  // 队列满：丢弃整轮 chunk（盘中数据最新优先）
      }
    }
    shm->Header().writer_heartbeat_epoch.store(
        static_cast<uint64_t>(now_ms / 1000), std::memory_order_release);
    return cnt;
  }

  // 同步路径（mmap_path 空）：港股不入库，无 mmap 时采集即弃。
  if (wm == WorkerMarket::HK) {
    std::cerr << "[HK] 港股不入库，需 --mmap_path 才能看盘（本次报价丢弃）\n";
    return cnt;
  }
  tdx::taos::TaosConfig tcfg = tdx::taos::TaosConfig::FromEnv();
  tdx::taos::TaosConnection tconn(tcfg);
  if (!tconn) { cnt.errors++; return cnt; }
  EnsureTables(tconn.native(), do_tx, do_tick, do_idx, do_unu, do_vol, do_hist);
  for (auto& ch : chunks) {
    IngestChunk(tconn.native(), ch, now_ms, day_ts,
                do_tx, do_tick, do_idx, do_unu, do_vol, do_hist, cnt);
  }
  return cnt;
}

// 异步入库线程（独立 std::thread，脱离 helio Proactor——taos_* 阻塞合规）。
// 设计文档 §D4 / §6.2。持独立 TaosConnection，drain SPSC 队列 → IngestChunk 落库。
// g_stop 退出主循环后，再 drain 一次确保队列内剩余 chunk 不丢。
void IngestWorker(tdx::shm::SegmentHeader* shm_header,
                  folly::ProducerConsumerQueue<FetchChunk>& q,
                  bool do_tx, bool do_tick, bool do_idx, bool do_unu,
                  bool do_vol, bool do_hist) {
  tdx::taos::TaosConfig tcfg = tdx::taos::TaosConfig::FromEnv();
  std::unique_ptr<tdx::taos::TaosConnection> conn;  // 惰性：延迟到首次有数据才连接，避免 taos_connect 阻塞 join
  bool tables_ok = false;
  auto drain_all = [&] {
    if (!conn) {
      conn = std::make_unique<tdx::taos::TaosConnection>(tcfg);
      if (!*conn) { conn.reset(); return; }  // 连接失败，下轮重试
      EnsureTables(conn->native(), do_tx, do_tick, do_idx, do_unu, do_vol, do_hist);
      tables_ok = true;
    }
    if (!tables_ok) return;
    FetchChunk ch;
    while (q.read(ch)) {
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      Counters dummy;
      IngestChunk(conn->native(), ch, now_ms, DayStartMs(),
                  do_tx, do_tick, do_idx, do_unu, do_vol, do_hist, dummy);
    }
    if (shm_header) shm_header->last_ingested_epoch.store(
        static_cast<uint64_t>(std::time(nullptr)), std::memory_order_release);
  };
  while (!g_stop) {
    drain_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 裸线程轮询，合规
  }
  drain_all();  // 收尾：drain g_stop 后队列内剩余 chunk
}

}  // namespace

int DoFetchQuotes(int /*argc*/, char** /*argv*/) {
  std::string codes_str = absl::GetFlag(FLAGS_quote_codes);
  int jobs = absl::GetFlag(FLAGS_quote_jobs);
  int batch_size = absl::GetFlag(FLAGS_quote_batch);
  bool loop = absl::GetFlag(FLAGS_quote_loop);
  int interval = absl::GetFlag(FLAGS_quote_interval);
  bool do_tx = absl::GetFlag(FLAGS_with_tx);
  bool do_tick = absl::GetFlag(FLAGS_with_tick);
  bool do_idx = absl::GetFlag(FLAGS_with_index);
  bool do_unu  = absl::GetFlag(FLAGS_with_unusual);
  bool do_vol  = absl::GetFlag(FLAGS_with_vol);
  bool do_hist = absl::GetFlag(FLAGS_with_hist);
  bool do_brd  = absl::GetFlag(FLAGS_with_board);
  std::string mmap_path = absl::GetFlag(FLAGS_mmap_path);
  bool do_hk = absl::GetFlag(FLAGS_quote_hk);

  if (do_brd) {
    std::cerr << "警告: --with_board 尚未实现，忽略。\n";
  }
  if (jobs < 1) jobs = 8;
  if (batch_size < 1) batch_size = 80;
  if (batch_size > 200) { std::cerr << "batch-size 上限 200\n"; batch_size = 200; }
  if (interval < 0) interval = 30;  // <0 兜底，0=连续无等待

  std::vector<std::string> codes;
  if (!codes_str.empty()) {
    codes = SplitCodes(codes_str);
    // 过滤非指数/个股/ETF 的代码
    codes.erase(std::remove_if(codes.begin(), codes.end(),
        [](const std::string& c) { return !IsQuoteTarget(c); }), codes.end());
    std::cerr << "指定代码: " << codes.size() << " 只\n";
  } else if (absl::GetFlag(FLAGS_all_market)) {
    std::cerr << "拉取全市场股票列表...\n";
    codes = FetchAllCodes();
    std::cerr << "全市场代码: " << codes.size() << " 只\n";
  } else {
    // 默认：采集自选股 zxg.blk（入库 + mmap 共享同一采集范围，需求 1/2 同时满足）
    std::string zxg = absl::GetFlag(FLAGS_zxg_blk);
    if (const char* e = std::getenv("TDX_ZXG_BLK"); e && *e) zxg = e;
    codes = ReadZxgBlk(zxg);
    codes.erase(std::remove_if(codes.begin(), codes.end(),
        [](const std::string& c) { return !IsQuoteTarget(c); }), codes.end());
    std::cerr << "自选股: " << codes.size() << " 只 (" << zxg << ")\n";
  }
  if (codes.empty()) { std::cerr << "没有股票代码可拉取\n"; return 1; }

  // 按 A 股/港股分桶。do_hk 开 + 代码是 HK（显式 sh/sz/bj 前缀排除）→ HK 桶，否则 A 股桶。
  std::vector<std::string> a_codes, hk_codes;
  for (auto& c : codes) {
    bool explicit_a = (c.size() >= 8) && (c.substr(0,2) == "sh" || c.substr(0,2) == "sz" || c.substr(0,2) == "bj");
    if (do_hk && !explicit_a && IsHkCode(c)) hk_codes.push_back(std::move(c));
    else a_codes.push_back(std::move(c));
  }
  if (a_codes.empty() && hk_codes.empty()) { std::cerr << "没有股票代码可拉取\n"; return 1; }

  std::cerr << "采集: quote";
  if (do_hk && !hk_codes.empty()) std::cerr << " +hk";
  if (do_tx) std::cerr << " +tx";
  if (do_tick) std::cerr << " +tick";
  if (do_idx) std::cerr << " +idx";
  if (do_unu) std::cerr << " +unu";
  if (do_vol) std::cerr << " +vol";
  if (do_hist) std::cerr << " +hist";
  if (do_brd) std::cerr << " +board";
  std::cerr << "  并发: " << jobs << "  循环: " << (loop ? "开" : "关");
  if (loop) std::cerr << "  间隔:" << interval << "s";
  if (!a_codes.empty()) std::cerr << "  A股:" << a_codes.size() << "只";
  if (!hk_codes.empty()) std::cerr << "  HK:" << hk_codes.size() << "只";
  std::cerr << "\n\n";

  // 异步入库：构造共享内存段 + SPSC 队列 + IngestWorker 线程（设计文档 §6.2）
  std::unique_ptr<tdx::shm::Segment> shm;
  std::unique_ptr<folly::ProducerConsumerQueue<FetchChunk>> ingest_q;
  std::thread ingest_worker;
  if (!mmap_path.empty()) {
    shm = tdx::shm::Segment::Create(mmap_path, tdx::shm::Layout{});
    if (!shm) { std::cerr << "创建共享内存段失败: " << mmap_path << "\n"; return 1; }
    ingest_q = std::make_unique<folly::ProducerConsumerQueue<FetchChunk>>(64);
    std::signal(SIGINT, OnSignal); std::signal(SIGTERM, OnSignal);  // 异步模式需 g_stop 控制 worker
    ingest_worker = std::thread(IngestWorker, &shm->Header(), std::ref(*ingest_q),
                                do_tx, do_tick, do_idx, do_unu, do_vol, do_hist);
    std::cerr << "异步入库已启用，共享内存: " << mmap_path << "\n";
  } else if (loop) {
    std::signal(SIGINT, OnSignal); std::signal(SIGTERM, OnSignal);
  }

  int round = 0;
  Counters total;
  do {
    ++round;
    auto t0 = std::chrono::steady_clock::now();

    if (!a_codes.empty()) {
      auto cnt = RunOneRound(a_codes, jobs, batch_size, WorkerMarket::AShare,
                             do_tx, do_tick, do_idx, do_unu,
                             do_vol, do_hist, shm.get(), ingest_q.get());
      total.quote += cnt.quote; total.tx += cnt.tx; total.tick += cnt.tick;
      total.index += cnt.index; total.unusual += cnt.unusual;
      total.vol += cnt.vol; total.hist += cnt.hist;
      total.dropped += cnt.dropped; total.skipped += cnt.skipped;
      total.errors += cnt.errors;

      std::cerr << "第" << round << "轮 A股:"
                << " quote=" << cnt.quote << " tx=" << cnt.tx
                << " tick=" << cnt.tick << " idx=" << cnt.index << " unu=" << cnt.unusual;
      if (cnt.vol) std::cerr << " vol=" << cnt.vol;
      if (cnt.hist) std::cerr << " hist=" << cnt.hist;
      if (cnt.errors) std::cerr << " err=" << cnt.errors;
      if (cnt.dropped) std::cerr << " dropped=" << cnt.dropped;
      if (cnt.skipped) std::cerr << " skip=" << cnt.skipped;
      std::cerr << "\n";
    }

    if (!hk_codes.empty()) {
      // HK 桶仅采集 quote（A 股子项 tx/tick/idx/vol/unu/hist 走 A 股协议，HK 不支持）。
      auto cnt = RunOneRound(hk_codes, jobs, batch_size, WorkerMarket::HK,
                             false, false, false, false, false, false,
                             shm.get(), ingest_q.get());
      total.quote += cnt.quote;
      total.errors += cnt.errors;
      total.dropped += cnt.dropped;
      total.skipped += cnt.skipped;

      std::cerr << "第" << round << "轮 HK:"
                << " quote=" << cnt.quote;
      if (cnt.errors) std::cerr << " err=" << cnt.errors;
      if (cnt.dropped) std::cerr << " dropped=" << cnt.dropped;
      if (cnt.skipped) std::cerr << " skip=" << cnt.skipped;
      std::cerr << "\n";
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (loop && !g_stop) {
      int64_t wait_s = interval - elapsed;
      if (wait_s > 0) {
        std::cerr << "等待 " << wait_s << "s ...\n";
        for (int64_t w = 0; w < wait_s && !g_stop; ++w)
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  } while (loop && !g_stop);

  // 异步入库收尾：通知 worker 退出并 join（drain 完剩余 chunk 后再汇总打印）
  if (ingest_worker.joinable()) {
    g_stop = true;
    ingest_worker.join();
  }

  std::cerr << "\n=== 采集结束 === 轮次:" << round
            << " quote:" << total.quote << " tx:" << total.tx
            << " tick:" << total.tick << " idx:" << total.index
            << " unu:" << total.unusual;
  if (total.vol) std::cerr << " vol:" << total.vol;
  if (total.hist) std::cerr << " hist:" << total.hist;
  if (total.dropped) std::cerr << " dropped:" << total.dropped;
  if (total.skipped) std::cerr << " skip:" << total.skipped;
  std::cerr << "\n";
  return 0;
}
