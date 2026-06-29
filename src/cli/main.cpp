// tdx 命令行工具。子命令：
//   tdx server-test                            测速标准行情服务器
//   tdx bars -s <code> -p <period> -n <count>  拉取 K 线并打印
//     period: 0=5分 1=15分 2=30分 3=60分 4=日 5=周 6=月 7=1分 9=多日 11=年
// CLI 在 main 线程（不在 fiber 内），通过 ProactorPool 调度 fiber 执行 IO。
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "base/init.h"
#include "util/fibers/pool.h"

#include "absl/flags/flag.h"

#include "tdx/consts.hpp"

ABSL_FLAG(uint32_t, jobs, 1, "import 并行线程数 (0=CPU 核数)");
#include "tdx/proto/server_pool.hpp"
#include "tdx/quotes/ext_quotes.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/data/tdx_data.hpp"
#include "tdx/batch/batch_fetch.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/taos/taos_import.hpp"
#include "tdx/util/time_util.hpp"

#include <set>
#include <taos.h>

using namespace tdx;

namespace {

int DoServerTest() {
  std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
  pool->Run();
  auto* pb = pool->GetNextProactor();
  auto hosts = quotes::StdQuotes::DefaultHosts();  // ponytail: 复用，消除重复
  std::cout << "测速 " << hosts.size() << " 个标准行情服务器...\n";
  for (const auto& h : hosts) {
    auto lat = pb->Await([&] { return proto::ServerPool::Probe(h, pb); });
    if (lat) {
      printf("%-24s %s:%-5d  %.2f ms\n", h.name.c_str(), h.ip.c_str(), h.port, *lat);
    } else {
      printf("%-24s %s:%-5d  不可达\n", h.name.c_str(), h.ip.c_str(), h.port);
    }
  }
  pool->Stop();
  return 0;
}

int DoBars(int argc, char** argv) {
  // 位置参数：tdx bars <code> <period> <count>（避免与 absl flag 解析冲突）
  std::string code = argc > 2 ? argv[2] : "600000";
  int period = argc > 3 ? std::atoi(argv[3]) : 4;  // DAILY
  int count = argc > 4 ? std::atoi(argv[4]) : 10;

  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) {
    std::cerr << "连接失败: " << ec.message() << "\n";
    return 1;
  }
  auto bars = sq.Bars(MarketFromCode(code), code, static_cast<Period>(period), 0, count);
  std::cout << "获取 " << bars.size() << " 根 K 线（" << code << "）\n";
  // 精度遵循全局规范：价位 %.2f，数量 %.0f
  for (const auto& b : bars) {
    auto c = tdx::util::epoch_to_cst(b.datetime);
    printf("%04d-%02d-%02d  开%.2f 高%.2f 低%.2f 收%.2f  量%.0f  额%.2f\n",
           c.year, c.month, c.day, b.open, b.high, b.low, b.close, b.volume, b.amount);
  }
  return 0;
}

// 扩展行情 K 线：tdx ex-bars <market> <code> <period> <count>
//   market：ExMarket 数字值（如 31=港股主板 47=沪深300期货 74=美股）
int DoExBars(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "用法: tdx ex-bars <market> <code> <period> <count>\n"
              << "  market: 31=港股主板 47=中金所期货 74=美股（ExMarket 值）\n";
    return 1;
  }
  int market = std::atoi(argv[2]);
  std::string code = argv[3];
  int period = argc > 4 ? std::atoi(argv[4]) : 4;
  int count = argc > 5 ? std::atoi(argv[5]) : 10;

  quotes::ExtQuotes eq;
  if (auto ec = eq.Connect()) {
    std::cerr << "扩展行情连接失败: " << ec.message() << "\n";
    return 1;
  }
  auto bars = eq.Bars(static_cast<ExMarket>(market), code, static_cast<Period>(period), 0, count);
  std::cout << "获取 " << bars.size() << " 根扩展 K线（" << code << "）\n";
  for (const auto& b : bars) {
    auto c = tdx::util::epoch_to_cst(b.datetime);
    printf("%04d-%02d-%02d  开%.2f 高%.2f 低%.2f 收%.2f  量%.0f\n",
           c.year, c.month, c.day, b.open, b.high, b.low, b.close, b.volume);
  }
  return 0;
}

// 统一 API：tdx fetch-history <code> [code...] [period]
//   period: 1d/5m/1m/15m/30m/1h（位置参数，避免 absl flag 冲突）
int DoFetchHistory(int argc, char** argv) {
  std::vector<std::string> codes;
  std::string period = "1d";
  for (int i = 2; i < argc; ++i) codes.emplace_back(argv[i]);
  // 末尾若是周期标识则视作 period
  if (codes.size() > 1) {
    const std::string& last = codes.back();
    if (last == "1d" || last == "5m" || last == "1m" || last == "15m" ||
        last == "30m" || last == "1h") {
      period = last;
      codes.pop_back();
    }
  }
  if (codes.empty()) {
    std::cerr << "用法: tdx fetch-history <code> [code...] [period]\n";
    return 1;
  }
  tdx::data::TdxData td;
  if (auto ec = td.Connect()) {
    std::cerr << "连接失败: " << ec.message() << "\n";
    return 1;
  }
  auto bars = td.FetchHistory(codes, "", "", period);
  std::cout << "获取 " << bars.size() << " 根 K线（" << codes.size() << " 只股票，period=" << period << "）\n";
  for (const auto& b : bars) {
    auto c = tdx::util::epoch_to_cst(b.datetime);
    printf("%04d-%02d-%02d  开%.2f 高%.2f 低%.2f 收%.2f  量%.0f\n",
           c.year, c.month, c.day, b.open, b.high, b.low, b.close, b.volume);
  }
  return 0;
}

// 并发批量拉取：tdx batch-fetch <code> [code...] [concurrency]
int DoBatchFetch(int argc, char** argv) {
  std::vector<std::string> codes;
  int concurrency = 4;
  for (int i = 2; i < argc; ++i) codes.emplace_back(argv[i]);
  if (!codes.empty()) {
    const std::string& last = codes.back();
    bool all_digit = !last.empty();
    for (char c : last) if (!std::isdigit(static_cast<unsigned char>(c))) all_digit = false;
    if (all_digit) { concurrency = std::atoi(last.c_str()); codes.pop_back(); }
  }
  if (codes.empty()) { std::cerr << "用法: tdx batch-fetch <code> [code...] [concurrency]\n"; return 1; }
  auto results = tdx::batch::BatchFetchKline(codes, concurrency, tdx::Period::DAILY, 0, 10);
  int ok = 0;
  for (const auto& r : results) {
    if (r.success) { ++ok; std::cout << r.code << ": " << r.bars.size() << " 根 K线\n"; }
    else std::cout << r.code << ": 失败\n";
  }
  std::cout << "完成: " << ok << "/" << results.size() << " 成功（并发=" << concurrency << "）\n";
  return 0;
}

// TDengine VARCHAR 字段：row[0] 指向数据，2字节LE长度前缀在 row[0]-2
static std::string ReadVarChar(void* col) {
  if (!col) return {};
  const auto* p = static_cast<const unsigned char*>(col);
  uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
  return std::string(reinterpret_cast<const char*>(p), len);
}

// 检查库中股票代码是否都有名称：tdx check-names
int DoCheckNames() {
  using tdx::taos::TaosConfig;
  using tdx::taos::TaosConnection;
  using tdx::taos::ExecSQL;

  TaosConfig cfg = TaosConfig::FromEnv();
  TaosConnection conn(cfg);
  if (!conn) {
    std::cerr << "TDengine 连接失败\n";
    return 1;
  }
  ExecSQL(conn.native(), "USE tdx");

  // 拉取 kline 中所有 distinct code（tag 查询）
  std::set<std::string> kline_codes;
  {
    TAOS_RES* res = ::taos_query(conn.native(), "SELECT DISTINCT code FROM kline");
    if (!res || ::taos_errno(res) != 0) {
      std::cerr << "查询 kline code 失败: " << (res ? ::taos_errstr(res) : "null") << "\n";
      ::taos_free_result(res);
      return 1;
    }
    TAOS_ROW row;
    while ((row = ::taos_fetch_row(res))) {
      if (row[0]) kline_codes.insert(ReadVarChar(row[0]));
    }
    ::taos_free_result(res);
  }

  // 拉取 stock_name 中所有 code（表不存在不视为错误）
  std::set<std::string> name_codes;
  {
    TAOS_RES* res = ::taos_query(conn.native(), "SELECT code FROM stock_name");
    if (res && ::taos_errno(res) == 0) {
      TAOS_ROW row;
      while ((row = ::taos_fetch_row(res))) {
        if (row[0]) name_codes.insert(ReadVarChar(row[0]));
      }
    }
    // 表不存在 = 空集合，后续报告为全部缺名称
    ::taos_free_result(res);
  }

  // 比较
  std::vector<std::string> missing_names;  // 有 K 线无名称
  std::vector<std::string> orphan_names;   // 有名称无 K 线
  for (const auto& c : kline_codes)
    if (!name_codes.count(c)) missing_names.push_back(c);
  for (const auto& c : name_codes)
    if (!kline_codes.count(c)) orphan_names.push_back(c);

  std::cout << "K 线代码:   " << kline_codes.size() << " 个\n"
            << "名称记录:   " << name_codes.size() << " 个\n";

  if (missing_names.empty() && orphan_names.empty()) {
    std::cout << "结论:       全部匹配 ✓\n";
  } else {
    if (!missing_names.empty()) {
      std::cout << "缺名称:     " << missing_names.size() << " 个\n";
      for (size_t i = 0; i < missing_names.size() && i < 20; ++i)
        std::cout << "  " << missing_names[i] << "\n";
      if (missing_names.size() > 20)
        std::cout << "  ... 共 " << missing_names.size() << " 个\n";
    }
    if (!orphan_names.empty()) {
      std::cout << "孤立名称:   " << orphan_names.size() << " 个\n";
      for (size_t i = 0; i < orphan_names.size() && i < 20; ++i)
        std::cout << "  " << orphan_names[i] << "\n";
      if (orphan_names.size() > 20)
        std::cout << "  ... 共 " << orphan_names.size() << " 个\n";
    }
  }
  return 0;
}

// 独立同步股票名称：tdx sync-names
int DoSyncNames() {
  using tdx::taos::TaosConfig;
  using tdx::taos::TaosConnection;
  using tdx::taos::ExecSQL;

  TaosConfig cfg = TaosConfig::FromEnv();
  TaosConnection conn(cfg);
  if (!conn) { std::cerr << "TDengine 连接失败\n"; return 1; }
  ExecSQL(conn.native(), "USE tdx");
  int n = tdx::taos::SyncStockNames(conn.native());
  std::cout << "同步完成: " << n << " 条名称\n";
  return 0;
}

// 清理非 A 股及退市标的：tdx cleanup
int DoCleanup() {
  using tdx::taos::TaosConfig;
  using tdx::taos::TaosConnection;
  using tdx::taos::ExecSQL;

  TaosConfig cfg = TaosConfig::FromEnv();
  TaosConnection conn(cfg);
  if (!conn) { std::cerr << "TDengine 连接失败\n"; return 1; }
  ExecSQL(conn.native(), "USE tdx");
  int n = tdx::taos::CleanupStaleCodes(conn.native());
  if (n < 0) { std::cerr << "清理失败（stock_name 表不存在？请先运行 sync-names）\n"; return 1; }
  std::cout << "清理完成: " << n << " 只过期标的\n";
  return 0;
}

}  // namespace

// import 子命令（定义在 cli/import.cpp，不在 namespace 内）
int DoImport(int argc, char** argv, int jobs);

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  if (argc < 2) {
    std::cerr << "用法:\n"
              << "  tdx server-test                    测速服务器\n"
              << "  tdx bars <code> <period> <count>   拉K线（如 tdx bars 600000 4 10）\n"
              << "  tdx ex-bars <market> <code> <period> <count>  扩展行情\n"
              << "  tdx fetch-history <code> [--period 1d]        统一API拉取+同步状态\n"
              << "  tdx import [taos] [codes...]  本地数据→TDengine\n"
              << "  tdx check-names                 检查代码名称完整性\n"
              << "  tdx sync-names                  独立同步代码→名称对照表\n"
              << "  tdx cleanup                     清理非A股/退市标的子表\n";
    return 1;
  }
  std::string cmd = argv[1];
  if (cmd == "server-test") return DoServerTest();
  if (cmd == "bars") return DoBars(argc, argv);
  if (cmd == "ex-bars") return DoExBars(argc, argv);
  if (cmd == "fetch-history") return DoFetchHistory(argc, argv);
  if (cmd == "batch-fetch") return DoBatchFetch(argc, argv);
  if (cmd == "import") return DoImport(argc, argv, static_cast<int>(absl::GetFlag(FLAGS_jobs)));
  if (cmd == "check-names") return DoCheckNames();
  if (cmd == "sync-names") return DoSyncNames();
  if (cmd == "cleanup") return DoCleanup();
  std::cerr << "未知命令: " << cmd << "\n";
  return 1;
}
