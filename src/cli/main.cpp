// tdx 命令行工具。子命令：
//   tdx server-test                            测速标准行情服务器
//   tdx bars -s <code> -p <period> -n <count>  拉取 K 线并打印
//     period: 0=5分 1=15分 2=30分 3=60分 4=日 5=周 6=月 7=1分 9=多日 11=年
// CLI 在 main 线程（不在 fiber 内），通过 ProactorPool 调度 fiber 执行 IO。
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/init.h"
#include "util/fibers/pool.h"

#include "absl/flags/flag.h"

#include "tdx/consts.hpp"

ABSL_FLAG(uint32_t, jobs, 16, "import 并行线程数 (0=CPU 核数)");
#include "tdx/proto/server_pool.hpp"
#include "tdx/proto/sp_parsers.hpp"
#include "tdx/quotes/ext_quotes.hpp"
#include "tdx/quotes/sp_quotes.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/data/tdx_data.hpp"
#include "tdx/batch/batch_fetch.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/taos/taos_import.hpp"
#include "tdx/data/scaling.hpp"
#include "tdx/util/code_validate.hpp"
#include "tdx/util/time_util.hpp"

#include <set>
#include <taos.h>

using namespace tdx;
using tdx::taos::ReadVarChar;

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
  if (concurrency < 1) concurrency = 4;  // 防御 atoi 失败或用户传 0
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

// 清空实时行情表：tdx truncate-quotes
int DoTruncateQuotes() {
  using tdx::taos::TaosConfig;
  using tdx::taos::TaosConnection;
  using tdx::taos::ExecSQL;

  TaosConfig cfg = TaosConfig::FromEnv();
  TaosConnection conn(cfg);
  if (!conn) { std::cerr << "TDengine 连接失败\n"; return 1; }
  ExecSQL(conn.native(), "USE tdx");
  ExecSQL(conn.native(), "DROP STABLE IF EXISTS tdx.quote");
  std::cout << "实时行情表已清空\n";
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

// 当日K线落库：拉最新 N 根 K 线（1d/5m/1m）写入 TDengine kline。
// 成交量单位为股（Vipdoc1d/VipdocMin volume 统一为 1.0，网络源直写），
// 与 vipdoc 导入（清库重导后）跨 cycle 一致，SUM(分钟)≈日线。
// ponytail: 重复运行当日 bar 会积重复行——清库重导前仅作补抓当日数据用。
int DoSyncKline(int argc, char** argv) {
  // 位置参数：tdx sync-kline <code> [code...] [periods=1d,5m,1m] [count=240]
  std::vector<std::string> codes;
  std::string periods = "1d,5m,1m";
  int count = 240;
  for (int i = 2; i < argc; ++i)
    codes.emplace_back(argv[i]);
  // 末位若为纯数字且非 6 位（排除股票代码） = count
  if (!codes.empty()) {
    const auto& last = codes.back();
    bool all_digit = !last.empty();
    for (char c : last) if (!std::isdigit(static_cast<unsigned char>(c))) all_digit = false;
    if (all_digit && last.size() != 6) { count = std::atoi(last.c_str()); codes.pop_back(); }
  }
  // 末位若为已知周期串 = periods override
  if (!codes.empty()) {
    const auto& last = codes.back();
    auto is_period = [](const std::string& s) {
      return s == "1d" || s == "5m" || s == "1m" || s.find(',') != std::string::npos;
    };
    if (is_period(last)) { periods = last; codes.pop_back(); }
  }
  if (codes.empty() || count <= 0) {
    std::cerr << "用法: tdx sync-kline <code> [code...] [1d|5m|1m|1d,5m,1m] [count]\n"
              << "  拉取最新 count 根 K 线写入 kline 表（默认三周期 240 根）\n";
    return 1;
  }

  // 成交量单位统一为股：Vipdoc1d/VipdocMin volume 均=1.0，网络直写不缩放。
  struct P { Period p; const char* tag; tdx::data::DataSource vsrc; };
  std::vector<P> ps;
  for (size_t s = 0; s < periods.size();) {
    size_t comma = periods.find(',', s);
    std::string tok = periods.substr(s, comma == std::string::npos ? std::string::npos : comma - s);
    if (tok == "1d") ps.push_back({Period::DAILY, "1d", tdx::data::DataSource::Vipdoc1d});
    else if (tok == "1m") ps.push_back({Period::MIN_1, "1m", tdx::data::DataSource::VipdocMin});
    else if (tok == "5m") ps.push_back({Period::MIN_5, "5m", tdx::data::DataSource::VipdocMin});
    if (comma == std::string::npos) break;
    s = comma + 1;
  }
  if (ps.empty()) { std::cerr << "无有效周期（支持 1d/5m/1m）\n"; return 1; }

  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) {
    std::cerr << "opentdx 连接失败: " << ec.message() << "\n";
    return 1;
  }
  using tdx::taos::TaosConfig;
  using tdx::taos::TaosConnection;
  using tdx::taos::ExecSQL;
  TaosConnection conn(TaosConfig::FromEnv());
  if (!conn) { std::cerr << "TDengine 连接失败\n"; return 1; }
  ExecSQL(conn.native(), "USE tdx");

  int64_t total = 0;
  for (const auto& code : codes) {
    if (!tdx::util::IsValidCode(code)) { std::cerr << code << ": 非法代码，跳过\n"; continue; }
    Market market = tdx::MarketFromCode(code);
    for (const auto& [period, tag, vsrc] : ps) {
      // opentdx 量按该周期历史源系数归一：日线 ×0.01（手），分钟 ×1.0（原样）。
      double vol_f = tdx::data::GetScaling(tdx::data::ClassifySecurity(code), vsrc).volume;
      auto bars = sq.Bars(market, code, period, 0, static_cast<uint16_t>(count));
      if (bars.empty()) { std::cerr << code << " " << tag << ": 无数据\n"; continue; }

      auto header = [](const std::string& c, const char* t) {
        std::ostringstream s;
        s << "INSERT INTO k_" << c << "_" << t
          << " USING kline TAGS('" << c << "','" << t << "') VALUES";
        return s.str();
      };
      std::ostringstream sql;
      sql << header(code, tag);
      int n = 0;
      for (const auto& b : bars) {
        if (b.datetime <= 0 || b.datetime > 4102444800LL) continue;  // 1970~2100
        if (std::isnan(b.open) || std::isnan(b.close) || std::isnan(b.high) ||
            std::isnan(b.low) || std::isnan(b.volume) || std::isnan(b.amount)) continue;
        char row[256];
        std::snprintf(row, sizeof(row), "(%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f)",
                      static_cast<long long>(b.datetime * 1000LL),
                      b.open, b.high, b.low, b.close, b.volume * vol_f, b.amount);
        sql << row;
        if (++n % 200 == 0) {  // ponytail: 分批避免单 SQL 过长
          if (!ExecSQL(conn.native(), sql.str().c_str()))
            std::cerr << code << " " << tag << " 批量写入失败\n";
          sql.str("");
          sql << header(code, tag);
        }
      }
      if (n % 200 != 0)
        if (!ExecSQL(conn.native(), sql.str().c_str()))
          std::cerr << code << " " << tag << " 写入失败\n";
      total += n;
      std::cout << code << " " << tag << ": " << n << " 根\n";
    }
  }
  std::cout << "=== sync-kline 完成，共 " << total << " 行 ===\n";
  return 0;
}

// 从网络拉取日线写入 TDengine（本地 vipdoc 无 .day 文件的代码补导）。
// 用法: tdx pull-kline <code> [code...]
int DoPullKline(int argc, char** argv) {
  std::vector<std::string> codes;
  for (int i = 2; i < argc; ++i) codes.emplace_back(argv[i]);
  if (codes.empty()) {
    std::cerr << "用法: tdx pull-kline <code> [code...]\n"
              << "  从网络拉取日线写入 TDengine（本地无 vipdoc 文件时使用）\n";
    return 1;
  }

  using tdx::taos::TaosConfig;
  using tdx::taos::TaosConnection;
  using tdx::taos::ExecSQL;
  TaosConfig cfg = TaosConfig::FromEnv();
  TaosConnection conn(cfg);
  if (!conn) { std::cerr << "TDengine 连接失败\n"; return 1; }
  ExecSQL(conn.native(), "USE tdx");

  auto result = tdx::taos::ImportKlineFromNetwork(conn.native(), codes);
  std::cout << "\n=== 网络补导完成 ===\n"
            << "股票: " << result.codes_ok << "/" << codes.size() << "\n"
            << "K线:  " << result.kline_rows << " 行\n";
  return (result.kline_rows >= 0) ? 0 : 1;
}

// ---- 财务 0x10 ----
int DoFinance(int argc, char** argv) {
  if (argc < 3) { std::cerr << "用法: tdx finance <code>\n"; return 1; }
  std::string code = argv[2];
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto f = sq.GetFinance(MarketFromCode(code), code);
  printf("%s 财务:\n", code.c_str());
  printf("  流通股本: %.0f万股  总股本: %.0f万股  每股收益: %.4f\n", f.liutongguben, f.zongguben, f.meigushouyi);
  printf("  上市日期: %u  行业: %d  省份: %d\n", f.ipo_date, f.industry, f.province);
  printf("  每股净资产: %.4f  营业收入: %.0f  净利润: %.0f\n", f.meigujinzichan, f.yinyezongshouru, f.guimujinlirun);
  return 0;
}

// ---- F10 0x2cf/0x2d0 ----
int DoF10(int argc, char** argv) {
  if (argc < 3) { std::cerr << "用法: tdx f10 <code>\n"; return 1; }
  std::string code = argv[2];
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto cats = sq.GetF10Category(MarketFromCode(code), code);
  std::cout << code << " F10 目录 (" << cats.size() << " 项):\n";
  for (const auto& c : cats)
    printf("  %s [%s] start=%u len=%u\n", c.name.c_str(), c.filename.c_str(), c.start, c.length);
  return 0;
}

// ---- 历史委托 0xfb4 ----
int DoHistoryOrders(int argc, char** argv) {
  if (argc < 4) { std::cerr << "用法: tdx history-orders <code> <YYYYMMDD>\n"; return 1; }
  std::string code = argv[2];
  uint32_t date = static_cast<uint32_t>(std::atol(argv[3]));
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto orders = sq.GetHistoryOrders(MarketFromCode(code), code, date);
  std::cout << code << " " << date << " 委托 (" << orders.size() << " 条):\n";
  for (const auto& o : orders)
    printf("  price=%.2f vol=%ld\n", o.price, (long)o.vol);
  return 0;
}

// ---- 历史逐笔 0xfb5 ----
int DoHistoryTx(int argc, char** argv) {
  if (argc < 4) { std::cerr << "用法: tdx history-tx <code> <YYYYMMDD>\n"; return 1; }
  std::string code = argv[2];
  uint32_t date = static_cast<uint32_t>(std::atol(argv[3]));
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto txns = sq.GetHistoryTransaction(MarketFromCode(code), code, date, 0, 2000);
  std::cout << code << " " << date << " 逐笔 (" << txns.size() << " 条):\n";
  for (size_t i = 0; i < std::min(txns.size(), size_t(20)); ++i) {
    const auto& t = txns[i];
    int h = t.minutes / 60 % 24, m = t.minutes % 60;
    printf("  %02d:%02d price=%.2f vol=%ld bs=%d\n", h, m, t.price, (long)t.vol, t.buy_sell);
  }
  if (txns.size() > 20) std::cout << "  ... 共 " << txns.size() << " 条\n";
  return 0;
}

// ---- 成交量分布 0x51a ----
int DoVolProfile(int argc, char** argv) {
  if (argc < 3) { std::cerr << "用法: tdx vol-profile <code>\n"; return 1; }
  std::string code = argv[2];
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto vp = sq.GetVolumeProfile(MarketFromCode(code), code);
  printf("%s 量价分布:\n", code.c_str());
  printf("  现价: %.2f O:%.2f H:%.2f L:%.2f 昨收:%.2f 量:%.0f 额:%.0f\n",
         vp.price, vp.open, vp.high, vp.low, vp.pre_close, vp.vol, vp.amount);
  for (const auto& l : vp.levels)
    printf("  P=%.2f vol=%ld buy=%ld sell=%ld\n", l.price, (long)l.vol, (long)l.buy, (long)l.sell);
  return 0;
}

// ---- 指数信息 0x51d ----
int DoIndexInfo(int argc, char** argv) {
  if (argc < 3) { std::cerr << "用法: tdx index-info <code>\n"; return 1; }
  std::string code = argv[2];
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto ii = sq.GetIndexInfo(MarketFromCode(code), code);
  printf("%s: 现价%.2f 昨收%.2f 涨跌%.2f O%.2f H%.2f L%.2f 量%.0f 额%.0f\n",
         code.c_str(), ii.close, ii.pre_close, ii.diff, ii.open, ii.high, ii.low, ii.vol, ii.amount);
  printf("  上涨%ld 下跌%ld\n", (long)ii.up_count, (long)ii.down_count);
  return 0;
}

// ---- 主力异动 0x563 ----
int DoUnusual(int argc, char** argv) {
  int market = argc > 2 ? std::atoi(argv[2]) : 1;
  quotes::StdQuotes sq;
  if (auto ec = sq.Connect()) { std::cerr << "连接失败: " << ec.message() << "\n"; return 1; }
  auto items = sq.GetUnusual(static_cast<Market>(market), 0, 600);
  std::cout << "异动 (" << items.size() << " 条):\n";
  for (const auto& u : items)
    printf("  %s %02d:%02d:%02d %s %s\n", u.code.c_str(),
           u.hour, u.minute, u.second, u.desc.c_str(), u.value_str.c_str());
  return 0;
}

// ---- 板块列表 0x1231 ----
int DoBoardList(int argc, char** argv) {
  int type = argc > 2 ? std::atoi(argv[2]) : 1;  // BoardType
  quotes::SPQuotes sp;
  if (auto ec = sp.Connect()) { std::cerr << "SP连接失败: " << ec.message() << "\n"; return 1; }
  auto body = proto::serialize_sp_board_list(static_cast<BoardType>(type), 0, 150);
  auto resp = sp.Call(proto::kMsgSpBoardList, body);
  if (resp.body.empty()) { std::cerr << "无数据\n"; return 0; }
  auto boards = proto::deserialize_sp_board_list(resp.body.data(), resp.body.size());
  std::cout << "板块 (" << boards.size() << " 个):\n";
  for (size_t i = 0; i < std::min(boards.size(), size_t(20)); ++i)
    printf("  %s %s %.2lf\n", boards[i].code.c_str(), boards[i].name.c_str(), boards[i].price);
  if (boards.size() > 20) std::cout << "  ... 共 " << boards.size() << " 个\n";
  return 0;
}

// ---- 板块成员报价 0x122C ----
int DoBoardQuotes(int argc, char** argv) {
  if (argc < 3) { std::cerr << "用法: tdx board-quotes <board_code>\n"; return 1; }
  std::string code = argv[2];
  quotes::SPQuotes sp;
  if (auto ec = sp.Connect()) { std::cerr << "SP连接失败: " << ec.message() << "\n"; return 1; }
  int board_code = std::atoi(code.c_str());
  auto body = proto::serialize_sp_board_members(board_code, SortType::Code, 0, 80, SortOrder::None, {});
  auto resp = sp.Call(proto::kMsgSpBoardMembers, body);
  if (resp.body.empty()) { std::cerr << "无数据\n"; return 0; }
  std::cout << "板块 " << code << " 成员报价请求已发送 (resp " << resp.body.size() << "B)\n";
  return 0;
}

// ---- 资金流向 0x1218 ----
int DoCapitalFlow(int argc, char** argv) {
  if (argc < 3) { std::cerr << "用法: tdx capital-flow <code>\n"; return 1; }
  std::string code = argv[2];
  quotes::SPQuotes sp;
  if (auto ec = sp.Connect()) { std::cerr << "SP连接失败: " << ec.message() << "\n"; return 1; }
  auto body = proto::serialize_sp_capital_flow(static_cast<uint16_t>(MarketFromCode(code)), code);
  auto resp = sp.Call(proto::kMsgSpCapitalFlow, body);
  if (resp.body.empty()) { std::cerr << "无数据\n"; return 0; }
  auto flows = proto::deserialize_sp_capital_flow(resp.body.data(), resp.body.size());
  std::cout << code << " 资金流向:\n";
  for (const auto& cf : flows)
    printf("  主力净:%.0lf 散户净:%.0lf 5日主力:%.0lf\n", cf.main_net, cf.small_net, cf.five_day_main);
  return 0;
}

}  // namespace

// import 子命令（定义在 cli/import.cpp，不在 namespace 内）
int DoImport(int argc, char** argv, int jobs);

// fetch-quotes 子命令（定义在 cli/fetch_quotes.cpp）
int DoFetchQuotes(int argc, char** argv);

// 在 absl ParseCommandLine (由 MainInitGuard 触发) 之前拦截 --help/-h/help，
// 否则 gflags 会吃掉 --help 直接 exit(0)，用户永远看不到子命令列表。
static void PrintUsage() {
  std::cerr << "用法:\n"
            << "  tdx server-test                    测速服务器\n"
            << "  tdx bars <code> <period> <count>   拉K线（如 tdx bars 600000 4 10）\n"
            << "  tdx ex-bars <market> <code> <period> <count>  扩展行情\n"
            << "  tdx fetch-history <code> [--period 1d]        统一API拉取+同步状态\n"
            << "  tdx import [taos] [codes...]  本地数据→TDengine\n"
            << "  tdx fetch-quotes [--loop] [--codes ...]    实时行情采集→TDengine\n"
            << "  tdx check-names                 检查代码名称完整性\n"
            << "  tdx sync-names                  独立同步代码→名称对照表\n"
            << "  tdx cleanup                     清理非A股/退市标的子表\n"
            << "  tdx truncate-quotes             清空实时行情表（DROP+重建）\n"
            << "  tdx pull-kline <code> [code...] 网络拉取日线→TDengine（补导缺失代码）\n"
            << "  tdx sync-kline <code> [code...] [periods] [count]  当日K线→TDengine（1d/5m/1m，盘中刷新）\n"
            << "  tdx finance <code>              财务数据\n"
            << "  tdx f10 <code>                  F10基本资料\n"
            << "  tdx history-orders <code> <date> 历史委托(YYYYMMDD)\n"
            << "  tdx history-tx <code> <date>     历史逐笔(YYYYMMDD)\n"
            << "  tdx vol-profile <code>           成交量分布\n"
            << "  tdx index-info <code>            指数信息\n"
            << "  tdx unusual [market=1]           主力异动\n"
            << "  tdx board-list [type=1]          板块列表\n"
            << "  tdx board-quotes <code>          板块成员报价\n"
            << "  tdx capital-flow <code>          资金流向\n";
}

int main(int argc, char** argv) {
  // 在 gflags 解析前拦截 help 请求
  if (argc > 1) {
    std::string a1 = argv[1];
    if (a1 == "--help" || a1 == "-h" || a1 == "help") {
      PrintUsage();
      return 0;
    }
  }

  MainInitGuard guard(&argc, &argv);
  if (argc < 2) {
    PrintUsage();
    return 1;
  }
  std::string cmd = argv[1];
  if (cmd == "server-test") return DoServerTest();
  if (cmd == "bars") return DoBars(argc, argv);
  if (cmd == "ex-bars") return DoExBars(argc, argv);
  if (cmd == "fetch-history") return DoFetchHistory(argc, argv);
  if (cmd == "batch-fetch") return DoBatchFetch(argc, argv);
  if (cmd == "import") return DoImport(argc, argv, static_cast<int>(absl::GetFlag(FLAGS_jobs)));
  if (cmd == "fetch-quotes") return DoFetchQuotes(argc, argv);
  if (cmd == "check-names") return DoCheckNames();
  if (cmd == "sync-names") return DoSyncNames();
  if (cmd == "cleanup") return DoCleanup();
  if (cmd == "truncate-quotes") return DoTruncateQuotes();
  if (cmd == "pull-kline") return DoPullKline(argc, argv);
  if (cmd == "sync-kline") return DoSyncKline(argc, argv);
  if (cmd == "finance") return DoFinance(argc, argv);
  if (cmd == "f10") return DoF10(argc, argv);
  if (cmd == "history-orders") return DoHistoryOrders(argc, argv);
  if (cmd == "history-tx") return DoHistoryTx(argc, argv);
  if (cmd == "vol-profile") return DoVolProfile(argc, argv);
  if (cmd == "index-info") return DoIndexInfo(argc, argv);
  if (cmd == "unusual") return DoUnusual(argc, argv);
  if (cmd == "board-list") return DoBoardList(argc, argv);
  if (cmd == "board-quotes") return DoBoardQuotes(argc, argv);
  if (cmd == "capital-flow") return DoCapitalFlow(argc, argv);
  std::cerr << "未知命令: " << cmd << "\n";
  return 1;
}
