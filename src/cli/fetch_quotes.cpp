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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <csignal>
#include <ctime>
#include <thread>
#include <iostream>
#include <memory>
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
#include "tdx/proto/server_pool.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/taos/taos_import.hpp"
#include "tdx/types.hpp"

#include <taos.h>

ABSL_FLAG(std::string, quote_codes, "", "股票代码列表（逗号分隔），不指定则拉取全市场");
ABSL_FLAG(int32_t, quote_jobs, 8, "并发 worker 数");
ABSL_FLAG(int32_t, quote_batch, 80, "单次网络请求股票数（≤200）");
ABSL_FLAG(bool, quote_loop, false, "循环采集模式");
ABSL_FLAG(int32_t, quote_interval, 30, "循环间隔（秒）");
ABSL_FLAG(bool, with_tx, false, "同步采集逐笔成交 0xfc5");
ABSL_FLAG(bool, with_tick, false, "同步采集分时图 0x537");
ABSL_FLAG(bool, with_index, false, "同步采集指数信息 0x51d");
ABSL_FLAG(bool, with_unusual, false, "同步采集主力异动 0x563");
ABSL_FLAG(bool, with_finance, false, "采集财务数据 0x10");
ABSL_FLAG(bool, with_f10, false, "采集F10资料 0x2cf/0x2d0");
ABSL_FLAG(bool, with_vol, false, "采集成交量分布 0x51a");
ABSL_FLAG(bool, with_hist, false, "采集历史委托+逐笔 0xfb4/0xfb5");
ABSL_FLAG(bool, with_board, false, "采集板块列表+资金流向 0x1231/0x1218");

using namespace tdx;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

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

std::vector<std::string> FetchAllCodes() {
  std::vector<std::string> all;
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return all; }
  for (auto market : {Market::SH, Market::SZ, Market::BJ}) {
    uint16_t total = sq.StockCount(market);
    for (uint16_t start = 0; start < total; start += 1600) {
      auto stocks = sq.Stocks(market, start, std::min<uint16_t>(1600, total - start));
      for (auto& s : stocks) if (tdx::taos::IsAStock(s.code)) all.push_back(s.code);
    }
  }
  sq.Close();
  return all;
}

std::string Esc(const std::string& s) {
  std::string out; out.reserve(s.size() + 4);
  for (char ch : s) { if (ch == '\'') out += "''"; else out += ch; }
  return out;
}

// ---- 建表 & INSERT 辅助 ----

void EnsureTables(TAOS* conn, bool do_tx, bool do_tick, bool do_idx, bool do_unu,
                  bool do_fin, bool do_f10, bool do_vol, bool do_hist) {
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
  if (do_fin) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS finance (ts TIMESTAMP, liutongguben DOUBLE, "
    "zongguben DOUBLE, meigushouyi DOUBLE, meigujinzichan DOUBLE, "
    "yinyezongshouru DOUBLE, guimujinlirun DOUBLE, ipo_date INT, industry INT) "
    "TAGS (code VARCHAR(10))");
  if (do_f10) tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS f10_cat (ts TIMESTAMP, cat_name VARCHAR(64), "
    "filename VARCHAR(80), start_pos INT, len_val INT) TAGS (code VARCHAR(10))");
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
  if (code != 0) { std::cerr << "DB err[" << code << "]\n"; ::taos_free_result(res); return -1; }
  int64_t n = ::taos_affected_rows(res);
  ::taos_free_result(res);
  return n;
}

int64_t InsertQuote(TAOS* conn, const std::vector<Quote>& quotes, int64_t now_ms) {
  if (quotes.empty()) return 0;
  auto V = [](double v) { return std::isnan(v) ? "NULL" : (std::ostringstream() << v).str(); };
  int64_t written = 0;
  for (size_t i = 0; i < quotes.size(); i += 200) {
    size_t end = std::min(i + 200, quotes.size());
    std::ostringstream sql; sql << "INSERT INTO ";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      sql << "q_" << quotes[j].code << " USING quote TAGS('" << Esc(quotes[j].code) << "')";
      const auto& q = quotes[j];
      int64_t ts = q.datetime > 0 ? q.datetime * 1000LL : now_ms;
      sql << " VALUES(" << ts << "," << q.price << "," << q.pre_close << "," << q.open
          << "," << q.high << "," << q.low << "," << q.volume << "," << q.amount;
      for (int k = 0; k < 5; ++k) sql << "," << V(q.bid[k]);
      for (int k = 0; k < 5; ++k) sql << "," << V(q.ask[k]);
      for (int k = 0; k < 5; ++k) sql << "," << V(q.bid_vol[k]);
      for (int k = 0; k < 5; ++k) sql << "," << V(q.ask_vol[k]);
      sql << ")";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

int64_t InsertTx(TAOS* conn, const std::vector<Transaction>& txns,
                  std::string_view code, int64_t day_ts) {
  if (txns.empty()) return 0;
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
  if (ticks.empty()) return 0;
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

// 判断是否为指数代码（仅指数收集 index_info 和 unusual）
bool IsIndexCode(std::string_view code) {
  if (code.size() < 2) return false;
  std::string h2(code.substr(0, 2));
  return h2 == "88" || h2 == "99" || h2 == "39" || (code.size() > 2 && code.substr(0, 3) == "399");
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
// ---- finance/F10/vol/hist INSERT helpers ----
int64_t InsertFinance(TAOS* conn, const Finance& f, int64_t now_ms) {
  std::ostringstream sql;
  sql << "INSERT INTO fn_" << f.code << " USING finance TAGS('" << Esc(f.code) << "') VALUES("
      << now_ms << "," << f.liutongguben << "," << f.zongguben << ","
      << f.meigushouyi << "," << f.meigujinzichan << ","
      << f.yinyezongshouru << "," << f.guimujinlirun << ","
      << (int)f.ipo_date << "," << f.industry << ")";
  return ExecSql(conn, sql.str());
}

int64_t InsertF10Cat(TAOS* conn, const std::vector<F10Category>& cats,
                      std::string_view code, int64_t now_ms) {
  if (cats.empty()) return 0; int64_t written = 0;
  for (size_t i = 0; i < cats.size(); i += 50) {
    size_t end = std::min(i + 50, cats.size());
    std::ostringstream sql; sql << "INSERT INTO ";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      sql << "fc_" << code << "_" << j << " USING f10_cat TAGS('" << Esc(std::string(code)) << "') VALUES("
          << now_ms << ",'" << Esc(cats[j].name) << "','" << Esc(cats[j].filename) << "',"
          << cats[j].start << "," << cats[j].length                    << ")";
    }
    int64_t n = ExecSql(conn, sql.str()); if (n < 0) break; written += static_cast<int64_t>(end - i);
  }
  return written;
}

int64_t InsertVol(TAOS* conn, const VolProfile& vp, int64_t now_ms) {
  std::ostringstream sql;
  sql << "INSERT INTO vl_" << vp.code << " USING vol TAGS('" << Esc(vp.code) << "') VALUES("
      << now_ms << "," << vp.price << "," << vp.pre_close << "," << vp.open
      << "," << vp.high << "," << vp.low << "," << vp.vol << "," << vp.amount << ")";
  return ExecSql(conn, sql.str());
}

int64_t InsertHistOrd(TAOS* conn, const std::vector<HistoryOrder>& orders,
                       std::string_view code, int64_t base_ts) {
  if (orders.empty()) return 0;
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
  if (txns.empty()) return 0;
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

// 统计计数器
struct Counters {
  int64_t quote = 0, tx = 0, tick = 0, index = 0, unusual = 0;
  int64_t finance = 0, f10 = 0, vol = 0, hist = 0;
  int errors = 0;
};

// worker 分片拉取
void WorkerRun(::util::fb2::ProactorBase* pb,
               ::util::FiberSocketBase::endpoint_type ep,
               const std::vector<std::string>& codes, int n, int batch_size,
               bool do_tx, bool do_tick, bool do_idx, bool do_unu,
               bool do_fin, bool do_f10, bool do_vol, bool do_hist,
               int wi, Counters& cnt, ::util::fb2::Mutex& mu) {
  proto::Connection conn(pb);
  if (conn.Connect(ep)) { std::lock_guard<::util::fb2::Mutex> lk(mu); cnt.errors++; return; }
  try {
    auto login = proto::serialize_login();
    auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                   login.data(), login.size());
    conn.Call(req);
  } catch (...) { std::lock_guard<::util::fb2::Mutex> lk(mu); cnt.errors++; return; }

  tdx::taos::TaosConfig tcfg = tdx::taos::TaosConfig::FromEnv();
  tdx::taos::TaosConnection tconn(tcfg);
  if (!tconn) { std::lock_guard<::util::fb2::Mutex> lk(mu); cnt.errors++; return; }
  EnsureTables(tconn.native(), do_tx, do_tick, do_idx, do_unu, do_fin, do_f10, do_vol, do_hist);

  Counters local;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  int64_t day_ts = DayStartMs();

  for (size_t bi = static_cast<size_t>(wi); bi < codes.size(); ) {
    if (g_stop) break;

    // ---- Quote (batch 0x53e) ----
    std::vector<proto::QuoteReq> batch;
    batch.reserve(batch_size);
    for (size_t k = 0; k < static_cast<size_t>(batch_size); ++k) {
      size_t ci = bi + k * static_cast<size_t>(n);
      if (ci >= codes.size()) break;
      batch.push_back({MarketFromCode(codes[ci]), codes[ci]});
    }
    bi += batch.size() * static_cast<size_t>(n);
    if (batch.empty()) continue;

    // 拉取 quote
    std::vector<Quote> quotes;
    try {
      auto body = proto::serialize_quotes_detail(batch);
      auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgQuotesDetail,
                              body.data(), body.size()));
      quotes = proto::deserialize_quotes_detail(resp.body.data(), resp.body.size());
    } catch (...) { local.errors++; }
    if (!quotes.empty()) {
      int64_t nrows = InsertQuote(tconn.native(), quotes, now_ms);
      if (nrows > 0) local.quote += nrows;
    }

    // ---- 每只股票单独拉取 transaction/tick/index/unusual ----
    for (const auto& req : batch) {
      if (g_stop) break;
      auto mkt = req.market;
      const auto& code = req.code;

      if (do_tx) {
        try {
          auto body = proto::serialize_transaction(mkt, code, 0, 2000);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgTransaction,
                                  body.data(), body.size()));
          auto txns = proto::deserialize_transaction(resp.body.data(), resp.body.size());
          if (!txns.empty()) local.tx += InsertTx(tconn.native(), txns, code, day_ts);
        } catch (...) {}
      }
      if (do_tick) {
        try {
          auto body = proto::serialize_tick(mkt, code, 0, kTickMaxCount);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgTick,
                                  body.data(), body.size()));
          auto ticks = proto::deserialize_tick(resp.body.data(), resp.body.size());
          if (!ticks.empty()) local.tick += InsertTick(tconn.native(), ticks, code, day_ts);
        } catch (...) {}
      }
      if (do_idx && IsIndexCode(code)) {
        try {
          auto body = proto::serialize_index_info(mkt, code);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgIndexInfo,
                                  body.data(), body.size()));
          auto ii = proto::deserialize_index_info(resp.body.data(), resp.body.size());
          if (!ii.code.empty()) local.index += InsertIdx(tconn.native(), ii, now_ms);
        } catch (...) {}
      }

      // ---- 每只股票：finance/F10/vol/hist（非批量，逐只拉取） ----
      if (do_fin) {
        try {
          auto f = proto::deserialize_finance(nullptr, 0); // placeholder
          // ponytail: finance uses direct std_quotes call, done in RunOneRound separately
        } catch (...) {}
      }
      if (do_vol) {
        try {
          auto body = proto::serialize_volume_profile(req.market, req.code);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgVolumeProfile,
                                  body.data(), body.size()));
          auto vp = proto::deserialize_volume_profile(resp.body.data(), resp.body.size());
          if (!vp.code.empty()) { InsertVol(tconn.native(), vp, now_ms); local.vol++; }
        } catch (...) {}
      }
    }

    // ---- 每只股票：finance(只拉一次的首仓拉) / F10 / hist ----
    if (do_fin && wi == 0) {
      for (const auto& req : batch) {
        try {
          auto body = proto::serialize_finance(req.market, req.code);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgFinance,
                                  body.data(), body.size()));
          auto f = proto::deserialize_finance(resp.body.data(), resp.body.size());
          if (!f.code.empty()) { InsertFinance(tconn.native(), f, now_ms); local.finance++; }
        } catch (...) {}
      }
    }
    if (do_f10 && wi == 0) {
      for (const auto& req : batch) {
        try {
          auto body = proto::serialize_f10_category(req.market, req.code);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgF10Category,
                                  body.data(), body.size()));
          auto cats = proto::deserialize_f10_category(resp.body.data(), resp.body.size());
          if (!cats.empty()) { InsertF10Cat(tconn.native(), cats, req.code, now_ms); local.f10++; }
        } catch (...) {}
      }
    }
    if (do_hist) {
      uint32_t today = 20260630; // ponytail: use current date
      for (const auto& req : batch) {
        try {
          auto body = proto::serialize_history_orders(req.market, req.code, today);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHistoryOrders,
                                  body.data(), body.size()));
          auto ord = proto::deserialize_history_orders(resp.body.data(), resp.body.size());
          if (!ord.empty()) local.hist += InsertHistOrd(tconn.native(), ord, req.code, day_ts);
        } catch (...) {}
        try {
          auto body = proto::serialize_history_transaction(req.market, req.code, today, 0, 2000);
          auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgHistoryTransaction,
                                  body.data(), body.size()));
          auto txns = proto::deserialize_history_transaction(resp.body.data(), resp.body.size());
          if (!txns.empty()) local.hist += InsertHistTx2(tconn.native(), txns, req.code, day_ts);
        } catch (...) {}
      }
    }

    // unusual 是市场级（非单只股票），只拉一次
    if (do_unu && wi == 0) {
      try {
        auto body = proto::serialize_unusual(Market::SH, 0, 600);
        auto resp = conn.Call(proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgUnusual,
                                body.data(), body.size()));
        auto items = proto::deserialize_unusual(resp.body.data(), resp.body.size());
        if (!items.empty()) local.unusual += InsertUnu(tconn.native(), items, now_ms);
      } catch (...) {}
    }
  }

  conn.Close();
  std::lock_guard<::util::fb2::Mutex> lk(mu);
  cnt.quote += local.quote; cnt.tx += local.tx; cnt.tick += local.tick;
  cnt.index += local.index; cnt.unusual += local.unusual; cnt.errors += local.errors;
  cnt.finance += local.finance; cnt.f10 += local.f10; cnt.vol += local.vol; cnt.hist += local.hist;
}

// 单轮采集：拉取全量行情 + 可选逐笔/分时/指数/异动 → 写 TDengine
Counters RunOneRound(const std::vector<std::string>& codes, int jobs, int batch_size,
                     bool do_tx, bool do_tick, bool do_idx, bool do_unu,
                     bool do_fin, bool do_f10, bool do_vol, bool do_hist) {
  Counters cnt;
  if (codes.empty()) return cnt;

  std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
  pool->Run();
  auto* pb = pool->GetNextProactor();

  proto::ServerPool sp(pool.get());
  auto best = sp.SelectBest(quotes::StdQuotes::DefaultHosts());
  if (!best) { std::cerr << "没有可用服务器\n"; pool->Stop(); cnt.errors++; return cnt; }
  std::cerr << "选服: " << best->name << " " << best->ip << ":" << best->port << "\n";

  auto addr = boost::asio::ip::make_address(best->ip);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  int n = std::min(jobs, static_cast<int>(codes.size()));
  if (n < 1) n = 1;

  ::util::fb2::Mutex mu;
  pb->Await([&] {
    std::vector<::util::fb2::Fiber> workers;
    workers.reserve(n);
    for (int wi = 0; wi < n; ++wi) {
      workers.push_back(::util::MakeFiber(
          ::util::fb2::Launch{.stack_size = 128 * 1024},
          [&, wi] { WorkerRun(pb, ep, codes, n, batch_size,
                              do_tx, do_tick, do_idx, do_unu,
                              do_fin, do_f10, do_vol, do_hist, wi, cnt, mu); }));
    }
    for (auto& w : workers) w.Join();
  });
  pool->Stop();
  return cnt;
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
  bool do_fin  = absl::GetFlag(FLAGS_with_finance);
  bool do_f10  = absl::GetFlag(FLAGS_with_f10);
  bool do_vol  = absl::GetFlag(FLAGS_with_vol);
  bool do_hist = absl::GetFlag(FLAGS_with_hist);
  bool do_brd  = absl::GetFlag(FLAGS_with_board);

  if (jobs < 1) jobs = 8;
  if (batch_size < 1) batch_size = 80;
  if (batch_size > 200) { std::cerr << "batch-size 上限 200\n"; batch_size = 200; }
  if (interval < 1) interval = 30;

  std::vector<std::string> codes;
  if (!codes_str.empty()) {
    codes = SplitCodes(codes_str);
    std::cerr << "指定代码: " << codes.size() << " 只\n";
  } else {
    std::cerr << "拉取全市场股票列表...\n";
    codes = FetchAllCodes();
    std::cerr << "全市场代码: " << codes.size() << " 只\n";
  }
  if (codes.empty()) { std::cerr << "没有股票代码可拉取\n"; return 1; }

  std::cerr << "采集: quote";
  if (do_tx) std::cerr << " +tx";
  if (do_tick) std::cerr << " +tick";
  if (do_idx) std::cerr << " +idx";
  if (do_unu) std::cerr << " +unu";
  if (do_fin) std::cerr << " +fin";
  if (do_f10) std::cerr << " +f10";
  if (do_vol) std::cerr << " +vol";
  if (do_hist) std::cerr << " +hist";
  if (do_brd) std::cerr << " +board";
  std::cerr << "  并发: " << jobs << "  循环: " << (loop ? "开" : "关");
  if (loop) std::cerr << "  间隔:" << interval << "s";
  std::cerr << "\n\n";

  if (loop) { std::signal(SIGINT, OnSignal); std::signal(SIGTERM, OnSignal); }

  int round = 0;
  Counters total;
  do {
    ++round;
    auto t0 = std::chrono::steady_clock::now();
    auto cnt = RunOneRound(codes, jobs, batch_size, do_tx, do_tick, do_idx, do_unu,
                           do_fin, do_f10, do_vol, do_hist);
    total.quote += cnt.quote; total.tx += cnt.tx; total.tick += cnt.tick;
    total.index += cnt.index; total.unusual += cnt.unusual;
    total.finance += cnt.finance; total.f10 += cnt.f10; total.vol += cnt.vol; total.hist += cnt.hist;

    std::cerr << "第" << round << "轮:"
              << " quote=" << cnt.quote << " tx=" << cnt.tx
              << " tick=" << cnt.tick << " idx=" << cnt.index << " unu=" << cnt.unusual;
    if (cnt.finance) std::cerr << " fin=" << cnt.finance;
    if (cnt.f10) std::cerr << " f10=" << cnt.f10;
    if (cnt.vol) std::cerr << " vol=" << cnt.vol;
    if (cnt.hist) std::cerr << " hist=" << cnt.hist;
    if (cnt.errors) std::cerr << " err=" << cnt.errors;
    std::cerr << "\n";

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

  std::cerr << "\n=== 采集结束 === 轮次:" << round
            << " quote:" << total.quote << " tx:" << total.tx
            << " tick:" << total.tick << " idx:" << total.index
            << " unu:" << total.unusual;
  if (total.finance) std::cerr << " fin:" << total.finance;
  if (total.f10) std::cerr << " f10:" << total.f10;
  if (total.vol) std::cerr << " vol:" << total.vol;
  if (total.hist) std::cerr << " hist:" << total.hist;
  std::cerr << "\n";
  return 0;
}
