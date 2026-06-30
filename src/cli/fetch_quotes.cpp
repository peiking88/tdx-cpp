// tdx fetch-quotes：实时行情采集 + TDengine 写入。支持单次 / 循环模式。
//
// 用法：
//   tdx fetch-quotes [--loop] [--interval 30] [--jobs 8] [--batch-size 80] [--codes 600000,000001]
//
// 架构：N fiber worker 分片 → 独立 Connection → batch serialize_quotes_detail(0x53e)
//       → 独立 TaosConnection → batch INSERT 入 quote STABLE。
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

// ponytail: 避免 Flag 名冲突（main.cpp 已定义 FLAGS_jobs），使用独特前缀
ABSL_FLAG(std::string, quote_codes, "", "股票代码列表（逗号分隔），不指定则拉取全市场");
ABSL_FLAG(int32_t, quote_jobs, 8, "并发 worker 数");
ABSL_FLAG(int32_t, quote_batch, 80, "单次网络请求股票数（≤200）");
ABSL_FLAG(bool, quote_loop, false, "循环采集模式");
ABSL_FLAG(int32_t, quote_interval, 30, "循环间隔（秒）");

using namespace tdx;

namespace {

// 全局退出标志（SIGINT 设置，循环模式用）
std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop = true; }

// 分割 "600000,000001,399001" → {"600000", "000001", "399001"}
std::vector<std::string> SplitCodes(const std::string& s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::string cur;
  for (char ch : s) {
    if (ch == ',') {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// 通过网络拉取全市场股票列表（SH/SZ/BJ，已 IsAStock 过滤）
std::vector<std::string> FetchAllCodes() {
  std::vector<std::string> all;
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) {
    std::cerr << "连接服务器失败: " << ec.message() << "\n";
    return all;
  }
  for (auto market : {Market::SH, Market::SZ, Market::BJ}) {
    uint16_t total = sq.StockCount(market);
    std::cerr << "市场 " << static_cast<int>(market) << " 总数: " << total << "\n";
    for (uint16_t start = 0; start < total; start += 1600) {
      auto stocks = sq.Stocks(market, start,
                              std::min<uint16_t>(1600, total - start));
      for (auto& s : stocks) {
        if (tdx::taos::IsAStock(s.code)) all.push_back(std::move(s.code));
      }
    }
  }
  sq.Close();
  return all;
}

// SQL 转义单引号
std::string Esc(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char ch : s) {
    if (ch == '\'') out += "''";
    else out += ch;
  }
  return out;
}

// 创建 quote STABLE 与子表（首次调用时）
void EnsureQuoteTable(TAOS* conn) {
  tdx::taos::ExecSQL(conn, "USE tdx");
  tdx::taos::ExecSQL(conn,
    "CREATE STABLE IF NOT EXISTS quote ("
    "ts TIMESTAMP, price DOUBLE, pre_close DOUBLE, open DOUBLE, "
    "high DOUBLE, low DOUBLE, volume DOUBLE, amount DOUBLE, "
    "bid1 DOUBLE, bid2 DOUBLE, bid3 DOUBLE, bid4 DOUBLE, bid5 DOUBLE, "
    "ask1 DOUBLE, ask2 DOUBLE, ask3 DOUBLE, ask4 DOUBLE, ask5 DOUBLE, "
    "bid_vol1 DOUBLE, bid_vol2 DOUBLE, bid_vol3 DOUBLE, bid_vol4 DOUBLE, bid_vol5 DOUBLE, "
    "ask_vol1 DOUBLE, ask_vol2 DOUBLE, ask_vol3 DOUBLE, ask_vol4 DOUBLE, ask_vol5 DOUBLE) "
    "TAGS (code VARCHAR(10))");
}

// 将 double 格式化为 SQL 值（NaN → NULL）
std::string SqlVal(double v) {
  if (std::isnan(v)) return "NULL";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  return buf;
}

// 写入一批 Quote 到 TDengine。TDengine 多子表 INSERT 语法（自动建子表）：
//   INSERT INTO q_A USING quote TAGS('A') VALUES(...)
//               q_B USING quote TAGS('B') VALUES(...)
int64_t InsertQuotes(TAOS* conn, const std::vector<Quote>& quotes, int64_t now_ms) {
  if (quotes.empty()) return 0;

  constexpr size_t kBatchInsert = 200;  // quote 列多(28列)，200 条/批控制 SQL 长度
  int64_t written = 0;

  for (size_t i = 0; i < quotes.size(); i += kBatchInsert) {
    size_t end = std::min(i + kBatchInsert, quotes.size());
    std::ostringstream sql;
    sql << "INSERT INTO ";
    for (size_t j = i; j < end; ++j) {
      if (j > i) sql << ' ';
      sql << "q_" << quotes[j].code;
      sql << " USING quote TAGS('" << Esc(quotes[j].code) << "')";
      const auto& q = quotes[j];
      int64_t ts = q.datetime > 0 ? q.datetime * 1000LL : now_ms;
      sql << " VALUES(" << ts
          << "," << q.price
          << "," << q.pre_close
          << "," << q.open
          << "," << q.high
          << "," << q.low
          << "," << q.volume
          << "," << q.amount;
      for (int k = 0; k < 5; ++k) sql << "," << SqlVal(q.bid[k]);
      for (int k = 0; k < 5; ++k) sql << "," << SqlVal(q.ask[k]);
      for (int k = 0; k < 5; ++k) sql << "," << SqlVal(q.bid_vol[k]);
      for (int k = 0; k < 5; ++k) sql << "," << SqlVal(q.ask_vol[k]);
      sql << ")";
    }

    TAOS_RES* res = ::taos_query(conn, sql.str().c_str());
    int code = ::taos_errno(res);
    if (code != 0) {
      std::cerr << "TDengine error [" << code << "]: " << ::taos_errstr(res)
                << "\n  SQL(len=" << sql.str().size() << ")\n";
      ::taos_free_result(res);
      return written > 0 ? written : -1;
    }
    int64_t n = ::taos_affected_rows(res);
    ::taos_free_result(res);
    if (n > 0) written += n;
  }
  return written;
}

// 单轮采集：拉取全量行情 + 写入 TDengine
// 返回本轮写入行数
int64_t RunOneRound(const std::vector<std::string>& codes, int jobs, int batch_size) {
  if (codes.empty()) return 0;

  std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
  pool->Run();
  auto* pb = pool->GetNextProactor();

  proto::ServerPool sp(pool.get());
  auto best = sp.SelectBest(quotes::StdQuotes::DefaultHosts());
  if (!best) {
    std::cerr << "没有可用服务器\n";
    pool->Stop();
    return -1;
  }
  std::cerr << "选服: " << best->name << " " << best->ip << ":" << best->port << "\n";

  auto addr = boost::asio::ip::make_address(best->ip);
  ::util::FiberSocketBase::endpoint_type ep(addr, best->port);

  int n = std::min(jobs, static_cast<int>(codes.size()));
  if (n < 1) n = 1;

  struct WorkerStat {
    int quotes_fetched = 0;
    int64_t rows_written = 0;
    int errors = 0;
  };
  ::util::fb2::Mutex mu;
  std::vector<WorkerStat> stats(n);

  pb->Await([&] {
    std::vector<::util::fb2::Fiber> workers;
    workers.reserve(n);
    for (int wi = 0; wi < n; ++wi) {
      workers.push_back(::util::MakeFiber([&, wi] {
        // ---- 网络连接 ----
        proto::Connection conn(pb);
        if (conn.Connect(ep)) { stats[wi].errors++; return; }
        try {
          auto login = proto::serialize_login();
          auto req = proto::pack_request(proto::kHeadNoZip, 0, proto::kMsgLogin,
                                         login.data(), login.size());
          conn.Call(req);
        } catch (...) { stats[wi].errors++; return; }

        // ---- TDengine 连接 ----
        tdx::taos::TaosConfig tcfg = tdx::taos::TaosConfig::FromEnv();
        tdx::taos::TaosConnection tconn(tcfg);
        if (!tconn) {
          std::cerr << "Worker " << wi << ": TDengine 连接失败\n";
          stats[wi].errors++;
          return;
        }
        EnsureQuoteTable(tconn.native());

        // ---- 分片拉取：worker wi 处理 codes[wi, wi+n, wi+2n, ...] ----
        int local_quotes = 0;
        int64_t local_rows = 0;
        for (size_t bi = static_cast<size_t>(wi); bi < codes.size(); ) {
          if (g_stop) break;

          // 收集 batch_size 个本 worker 的股票（codes[bi], codes[bi+n], ...）
          std::vector<proto::QuoteReq> batch;
          batch.reserve(batch_size);
          for (size_t k = 0; k < static_cast<size_t>(batch_size); ++k) {
            size_t ci = bi + k * static_cast<size_t>(n);
            if (ci >= codes.size()) break;
            batch.push_back({MarketFromCode(codes[ci]), codes[ci]});
          }
          bi += batch.size() * static_cast<size_t>(n);  // 跳过已处理的代码

          if (batch.empty()) continue;

          std::vector<Quote> quotes;
          try {
            auto body = proto::serialize_quotes_detail(batch);
            auto resp = conn.Call(proto::pack_request(
                proto::kHeadNoZip, 0, proto::kMsgQuotesDetail,
                body.data(), body.size()));
            quotes = proto::deserialize_quotes_detail(resp.body.data(),
                                                       resp.body.size());
          } catch (const std::exception& e) {
            std::cerr << "Worker " << wi << " 请求失败: " << e.what() << "\n";
            stats[wi].errors++;
            continue;
          }

          if (quotes.empty()) continue;
          local_quotes += static_cast<int>(quotes.size());

          // 写入 TDengine（当前毫秒作为 fallback ts）
          int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
          int64_t rows = InsertQuotes(tconn.native(), quotes, now_ms);
          if (rows > 0) local_rows += rows;
        }

        conn.Close();
        stats[wi].quotes_fetched = local_quotes;
        stats[wi].rows_written = local_rows;
      }));
    }
    for (auto& w : workers) w.Join();
  });

  pool->Stop();

  // 汇总统计
  int64_t total_rows = 0;
  int total_quotes = 0, total_errs = 0;
  for (const auto& s : stats) {
    total_quotes += s.quotes_fetched;
    total_rows += s.rows_written;
    total_errs += s.errors;
  }
  std::cerr << "本轮: " << total_quotes << " 条行情, "
            << total_rows << " 行写入";
  if (total_errs > 0) std::cerr << ", " << total_errs << " worker 异常";
  std::cerr << "\n";
  return total_rows;
}

}  // namespace

int DoFetchQuotes(int /*argc*/, char** /*argv*/) {
  std::string codes_str = absl::GetFlag(FLAGS_quote_codes);
  int jobs = absl::GetFlag(FLAGS_quote_jobs);
  int batch_size = absl::GetFlag(FLAGS_quote_batch);
  bool loop = absl::GetFlag(FLAGS_quote_loop);
  int interval = absl::GetFlag(FLAGS_quote_interval);

  if (jobs < 1) jobs = 8;
  if (batch_size < 1) batch_size = 80;
  if (batch_size > 200) {
    std::cerr << "batch-size 上限 200（避免服务器响应过大）\n";
    batch_size = 200;
  }
  if (interval < 1) interval = 30;

  // 获取代码列表
  std::vector<std::string> codes;
  if (!codes_str.empty()) {
    codes = SplitCodes(codes_str);
    std::cerr << "指定代码: " << codes.size() << " 只\n";
  } else {
    std::cerr << "拉取全市场股票列表...\n";
    codes = FetchAllCodes();
    std::cerr << "全市场代码: " << codes.size() << " 只\n";
  }
  if (codes.empty()) {
    std::cerr << "没有股票代码可拉取\n";
    return 1;
  }

  std::cerr << "并发 worker: " << jobs
            << ", batch-size: " << batch_size
            << ", 循环: " << (loop ? "开" : "关");
  if (loop) std::cerr << ", 间隔: " << interval << "s";
  std::cerr << "\n\n";

  // 循环模式：注册 SIGINT
  if (loop) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
  }

  int round = 0;
  int64_t grand_total = 0;
  do {
    ++round;
    auto t0 = std::chrono::steady_clock::now();

    int64_t rows = RunOneRound(codes, jobs, batch_size);
    if (rows < 0) {
      std::cerr << "第 " << round << " 轮失败\n";
      if (!loop) return 1;
    } else {
      grand_total += rows;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (loop && !g_stop) {
      int64_t wait_s = interval - elapsed;
      if (wait_s > 0) {
        std::cerr << "等待 " << wait_s << "s ...\n";
        for (int64_t w = 0; w < wait_s && !g_stop; ++w)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        // 注意：主线程不在 fiber 内，std::this_thread::sleep_for 安全
      }
    }
  } while (loop && !g_stop);

  std::cerr << "\n=== 采集结束 ===\n"
            << "轮次: " << round << "\n"
            << "累计写入: " << grand_total << " 行\n"
            << "目标库:   tdx.quote\n";
  return 0;
}
