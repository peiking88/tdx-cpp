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

#include "tdx/consts.hpp"
#include "tdx/proto/server_pool.hpp"
#include "tdx/quotes/ext_quotes.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/data/tdx_data.hpp"
#include "tdx/query/duckdb_query.hpp"
#include "tdx/util/time_util.hpp"

using namespace tdx;

namespace {

// 内置标准行情服务器（与 StdQuotes::DefaultHosts 一致）
std::vector<proto::ServerInfo> DefaultHosts() {
  return {
      {"通达信深圳双线主站1", "110.41.147.114", 7709},
      {"通达信深圳双线主站2", "110.41.2.72", 7709},
      {"通达信深圳双线主站3", "110.41.4.4", 7709},
      {"通达信上海双线主站1", "124.70.176.52", 7709},
      {"通达信上海双线主站2", "122.51.120.217", 7709},
      {"通达信上海双线主站3", "123.60.186.45", 7709},
      {"通达信北京双线主站1", "121.36.54.217", 7709},
      {"通达信广州双线主站1", "124.71.85.110", 7709},
      {"通达信武汉电信主站1", "119.97.185.59", 7709},
  };
}

int DoServerTest() {
  std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
  pool->Run();
  auto* pb = pool->GetNextProactor();
  auto hosts = DefaultHosts();
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

// DuckDB 即席 SQL 查询：tdx sql "SELECT ..."
int DoSql(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: tdx sql \"<SQL>\"（如 tdx sql \"SELECT * FROM 'k.parquet'\"）\n";
    return 1;
  }
  tdx::query::DuckDBQuery q;
  auto n = q.Exec(argv[2]);
  std::cout << (n >= 0 ? "OK rows=" + std::to_string(n) : "ERROR") << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);
  if (argc < 2) {
    std::cerr << "用法:\n"
              << "  tdx server-test                    测速服务器\n"
              << "  tdx bars <code> <period> <count>   拉K线（如 tdx bars 600000 4 10）\n"
              << "  tdx ex-bars <market> <code> <period> <count>  扩展行情\n"
              << "  tdx fetch-history <code> [--period 1d]        统一API拉取+同步状态\n";
    return 1;
  }
  std::string cmd = argv[1];
  if (cmd == "server-test") return DoServerTest();
  if (cmd == "bars") return DoBars(argc, argv);
  if (cmd == "ex-bars") return DoExBars(argc, argv);
  if (cmd == "fetch-history") return DoFetchHistory(argc, argv);
  if (cmd == "sql") return DoSql(argc, argv);
  std::cerr << "未知命令: " << cmd << "\n";
  return 1;
}
