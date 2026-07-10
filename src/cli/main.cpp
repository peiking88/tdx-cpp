// tdx 命令行工具。子命令见 PrintUsage()。
// bars / ex-bars 已回退为测试用例（tests/test_bars.cpp）。
// CLI 在 main 线程（不在 fiber 内），通过 ProactorPool 调度 fiber 执行 IO。
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "base/init.h"
#include "util/fibers/pool.h"

#include "absl/flags/flag.h"
#include "absl/strings/numbers.h"

#include "tdx/consts.hpp"

ABSL_FLAG(uint32_t, jobs, 4, "import 并行线程数 (0=CPU 核数)");
ABSL_FLAG(int32_t, kline_interval, 60, "fetch-kline 循环间隔（秒）");
#include "tdx/proto/server_pool.hpp"
#include "tdx/proto/vipdoc_reader.hpp"
#include "tdx/proto/sp_parsers.hpp"
#include "tdx/quotes/ext_quotes.hpp"
#include "tdx/quotes/sp_quotes.hpp"
#include "tdx/quotes/std_quotes.hpp"
#include "tdx/data/tdx_data.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/taos/taos_import.hpp"
#include "tdx/data/scaling.hpp"
#include "tdx/util/code_validate.hpp"
#include "tdx/util/time_util.hpp"

#include <atomic>
#include <csignal>
#include <set>
#include <taos.h>

using namespace tdx;
using tdx::taos::ReadVarChar;

// fetch-kline 循环模式退出标志（SIGINT/SIGTERM 触发）。
namespace
{
  std::atomic<bool> g_fetch_kline_stop{false};
  void OnFetchKlineSignal(int) { g_fetch_kline_stop = true; }
}

namespace
{

  int DoServerTest()
  {
    std::unique_ptr<::util::ProactorPool> pool(::util::fb2::Pool::IOUring(64));
    pool->Run();
    auto *pb = pool->GetNextProactor();
    auto hosts = quotes::StdQuotes::DefaultHosts(); // ponytail: 复用，消除重复
    std::cout << "测速 " << hosts.size() << " 个标准行情服务器...\n";
    for (const auto &h : hosts)
    {
      auto lat = pb->Await([&]
                           { return proto::ServerPool::Probe(h, pb); });
      if (lat)
      {
        printf("%-24s %s:%-5d  %.2f ms\n", h.name.c_str(), h.ip.c_str(), h.port, *lat);
      }
      else
      {
        printf("%-24s %s:%-5d  不可达\n", h.name.c_str(), h.ip.c_str(), h.port);
      }
    }
    pool->Stop();
    return 0;
  }

  // bars / ex-bars / fetch-history 已回退为测试用例（tests/test_bars.cpp / test_fetch_history.cpp）。
  // 检查库中股票代码是否都有名称：tdx check-names
  int DoCheckNames()
  {
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;

    // 拉取 kline 中所有 distinct code（tag 查询）
    std::set<std::string> kline_codes;
    {
      TAOS_RES *res = ::taos_query(conn->native(), "SELECT DISTINCT code FROM kline");
      if (!res || ::taos_errno(res) != 0)
      {
        std::cerr << "查询 kline code 失败: " << (res ? ::taos_errstr(res) : "null") << "\n";
        ::taos_free_result(res);
        return 1;
      }
      TAOS_ROW row;
      while ((row = ::taos_fetch_row(res)))
      {
        if (row[0])
          kline_codes.insert(ReadVarChar(row[0]));
      }
      ::taos_free_result(res);
    }

    // 拉取 stock_name 中所有 code（表不存在不视为错误）
    std::set<std::string> name_codes;
    {
      TAOS_RES *res = ::taos_query(conn->native(), "SELECT code FROM stock_name");
      if (res && ::taos_errno(res) == 0)
      {
        TAOS_ROW row;
        while ((row = ::taos_fetch_row(res)))
        {
          if (row[0])
            name_codes.insert(ReadVarChar(row[0]));
        }
      }
      // 表不存在 = 空集合，后续报告为全部缺名称
      ::taos_free_result(res);
    }

    // 比较
    std::vector<std::string> missing_names; // 有 K 线无名称
    std::vector<std::string> orphan_names;  // 有名称无 K 线
    for (const auto &c : kline_codes)
      if (!name_codes.count(c))
        missing_names.push_back(c);
    for (const auto &c : name_codes)
      if (!kline_codes.count(c))
        orphan_names.push_back(c);

    std::cout << "K 线代码:   " << kline_codes.size() << " 个\n"
              << "名称记录:   " << name_codes.size() << " 个\n";

    if (missing_names.empty() && orphan_names.empty())
    {
      std::cout << "结论:       全部匹配 ✓\n";
    }
    else
    {
      if (!missing_names.empty())
      {
        std::cout << "缺名称:     " << missing_names.size() << " 个\n";
        for (size_t i = 0; i < missing_names.size() && i < 20; ++i)
          std::cout << "  " << missing_names[i] << "\n";
        if (missing_names.size() > 20)
          std::cout << "  ... 共 " << missing_names.size() << " 个\n";
      }
      if (!orphan_names.empty())
      {
        std::cout << "孤立名称:   " << orphan_names.size() << " 个\n";
        for (size_t i = 0; i < orphan_names.size() && i < 20; ++i)
          std::cout << "  " << orphan_names[i] << "\n";
        if (orphan_names.size() > 20)
          std::cout << "  ... 共 " << orphan_names.size() << " 个\n";
      }
    }
    return 0;
  }

  // 独立同步股票名称：tdx fetch-names
  int DoFetchNames()
  {
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;
    int n = tdx::taos::SyncStockNames(conn->native());
    std::cout << "同步完成: " << n << " 条名称\n";
    return 0;
  }

  // 清空实时行情表：tdx truncate-quotes
  int DoTruncateQuotes()
  {
    using tdx::taos::ExecSQL;
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;
    ExecSQL(conn->native(), "DROP STABLE IF EXISTS tdx.quote");
    std::cout << "实时行情表已清空\n";
    return 0;
  }

  // 清理非 A 股及退市标的：tdx cleanup
  int DoCleanup()
  {
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;
    int n = tdx::taos::CleanupStaleCodes(conn->native());
    if (n < 0)
    {
      std::cerr << "清理失败（stock_name 表不存在？请先运行 fetch-names）\n";
      return 1;
    }
    std::cout << "清理完成: " << n << " 只过期标的\n";
    return 0;
  }

  // 当日K线落库：拉最新 N 根 K 线（1d/5m/1m）写入 TDengine kline。
  // 成交量单位为股（个股/基金直写，指数日线手→股×100 对齐 vipdoc），与 vipdoc 导入跨 cycle 一致，SUM(分钟)≈日线。
  // TDengine 子表以时间戳为主键——每轮重写当日 bar 是 upsert(覆盖)，幂等，不积重复行。
  int DoFetchKline(int argc, char **argv)
  {
    // 位置参数：tdx fetch-kline <code> [code...] [periods=1d,5m,1m] [count=240]
    // 默认循环模式（--kline_interval=60s），收盘后 3 轮无新 bar 退出。
    // absl flag（--kline_interval）遇子命令后不再解析，留在 argv 中，
    // 这里跳过以 '-' 开头的参数，避免被误当 code。
    std::vector<std::string> codes;
    std::string periods = "1d,5m,1m";
    int count = 240;
    for (int i = 2; i < argc; ++i)
    {
      std::string a = argv[i];
      if (!a.empty() && a[0] == '-')
        continue; // 跳过 absl flag
      codes.emplace_back(std::move(a));
    }
    // 末位若为纯数字且非 6 位（排除股票代码） = count
    if (!codes.empty())
    {
      const auto &last = codes.back();
      bool all_digit = !last.empty();
      for (char c : last)
        if (!std::isdigit(static_cast<unsigned char>(c)))
          all_digit = false;
      if (all_digit && last.size() != 6)
      {
        (void)absl::SimpleAtoi(last, &count);  // 溢出→保留默认值 240
        codes.pop_back();
      }
    }
    // 末位若为已知周期串 = periods override
    if (!codes.empty())
    {
      const auto &last = codes.back();
      auto is_period = [](const std::string &s)
      {
        return s == "1d" || s == "5m" || s == "1m" || s.find(',') != std::string::npos;
      };
      if (is_period(last))
      {
        periods = last;
        codes.pop_back();
      }
    }
    if (codes.empty() || count <= 0)
    {
      std::cerr << "用法: tdx fetch-kline <code> [code...] [1d|5m|1m|1d,5m,1m] [count]\n"
                << "  循环拉取当日 K 线写入 kline 表（默认三周期 240 根，间隔 --kline_interval 秒）\n"
                << "  code 可带市场前缀 sh/sz/bj（sh000001=上证指数；000001 默认 SZ=平安银行）\n";
      return 1;
    }

    // ponytail: vsrc 借用 Vipdoc* 系数表凑「网络源直写」效果——scaling.hpp 对网络K线无按类别×
    // 周期独立条目，而 Vipdoc1d 恰个股1.0/指数100.0、VipdocMin 全1.0，与网络源 volume 语义一致。
    // 升级路径=给 NetKlineStd 增加按类别 volume 条目后改用之；此处仅标注借用，不改计算结果。
    struct KlineCycle
    {
      Period p;
      const char *tag;
      tdx::data::DataSource vsrc;
    };
    std::vector<KlineCycle> cycles;
    for (size_t s = 0; s < periods.size();)
    {
      size_t comma = periods.find(',', s);
      std::string tok = periods.substr(s, comma == std::string::npos ? std::string::npos : comma - s);
      if (tok == "1d")
        cycles.push_back({Period::DAILY, "1d", tdx::data::DataSource::Vipdoc1d});
      else if (tok == "1m")
        cycles.push_back({Period::MIN_1, "1m", tdx::data::DataSource::VipdocMin});
      else if (tok == "5m")
        cycles.push_back({Period::MIN_5, "5m", tdx::data::DataSource::VipdocMin});
      if (comma == std::string::npos)
        break;
      s = comma + 1;
    }
    if (cycles.empty())
    {
      std::cerr << "无有效周期（支持 1d/5m/1m）\n";
      return 1;
    }

    // 非交易日（周末/节假日）直接退出，避免空转到 15:00。cfg/holidays.json 缺失则降级为仅判周末。
    {
      tdx::data::Calendar cal;
      auto c = tdx::util::epoch_to_cst(std::time(nullptr));
      if (!cal.IsTradingDay(c.year, c.month, c.day))
      {
        std::cerr << "今日(" << c.year << "-" << c.month << "-" << c.day
                  << ")非 A 股交易日，无需拉取，退出\n";
        return 0;
      }
    }

    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "opentdx 连接失败: " << ec.message() << "\n";
      return 1;
    }
    using tdx::taos::ExecSQL;
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;
    tdx::taos::EnsureKlineTables(conn->native());  // 新库首次运行时建超级表

    // 当日 00:00 CST epoch：仅保留当日盘中 bar，历史 K 线由 vipdoc 导入。
    const int64_t today_midnight_epoch = tdx::util::TodayMidnightEpoch();

    // 当日入库 bar 去重键（跨轮累积，TDengine upsert 幂等）：seen_keys.size() = 当日实际 bar 数。
    std::set<std::string> seen_keys;

    int interval = absl::GetFlag(FLAGS_kline_interval);
    if (interval <= 0)
      interval = 60;

    // 单轮：拉所有 code×period 的当日 bar 写入 TDengine，返回本轮写入 bar 数与最新 ts。
    // 用返回值判断"是否还有新 bar"（连续 3 轮无新 + 收盘后退出）。
    auto run_once = [&]() -> std::pair<int, int64_t>
    {
      int round_n = 0;
      int64_t round_max_ts = 0;
      for (const auto &raw : codes)
      {
        auto [market, code] = tdx::ParseMarketCode(raw);
        if (!tdx::util::IsValidCode(code))
        {
          std::cerr << raw << ": 非法代码，跳过\n";
          continue;
        }
        for (const auto &[period, tag, vsrc] : cycles)
        {
          double vol_f = tdx::data::GetScaling(tdx::data::ClassifySecurity(code, market), vsrc).volume;
          auto bars = sq.Bars(market, code, period, 0, static_cast<uint16_t>(count));
          if (bars.empty())
            continue;

          std::string mp = tdx::proto::VipdocReader::MarketDir(market);
          auto header = [&mp](const std::string &c, const char *t)
          {
            std::ostringstream s;
            s << "INSERT INTO k_" << mp << c << "_" << t
              << " USING kline TAGS('" << c << "','" << t << "','" << mp << "') VALUES";
            return s.str();
          };
          std::ostringstream sql;
          sql << header(code, tag);
          int n = 0;
          for (const auto &b : bars)
          {
            if (b.datetime <= 0 || b.datetime > 4102444800LL)
              continue; // 1970~2100
            if (std::isnan(b.open) || std::isnan(b.close) || std::isnan(b.high) ||
                std::isnan(b.low) || std::isnan(b.volume) || std::isnan(b.amount) ||
                std::isinf(b.open) || std::isinf(b.close) || std::isinf(b.high) ||
                std::isinf(b.low) || std::isinf(b.volume) || std::isinf(b.amount))
              continue;
            if (b.datetime < today_midnight_epoch)
              continue; // 仅当日
            if (!tdx::util::IsBarInTradingHours(b.datetime, period))
              continue; // D7 交易时段
            if (b.open <= 0 || b.close <= 0 || b.high <= 0 || b.low <= 0)
              continue;
            if (b.high < b.low)
              continue;
            if (b.volume < 0 || b.amount < 0)
              continue;
            char row[256];
            std::snprintf(row, sizeof(row), "(%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f)",
                          static_cast<long long>(b.datetime * 1000LL),
                          b.open, b.high, b.low, b.close, b.volume * vol_f, b.amount);
            sql << row;
            seen_keys.insert(code + "|" + tag + "|" + std::to_string(b.datetime));
            if (b.datetime > round_max_ts)
              round_max_ts = b.datetime;
            if (++n % 200 == 0)
            { // ponytail: 分批避免单 SQL 过长
              if (!ExecSQL(conn->native(), sql.str().c_str()))
                std::cerr << code << " " << tag << " 批量写入失败\n";
              sql.str("");
              sql << header(code, tag);
            }
          }
          if (n % 200 != 0)
            if (!ExecSQL(conn->native(), sql.str().c_str()))
              std::cerr << code << " " << tag << " 写入失败\n";
          round_n += n;
        }
      }
      return {round_n, round_max_ts};
    };

    // 当前 CST 时刻是否已过收盘（>=15:00）。
    auto market_closed = []()
    {
      auto c = tdx::util::epoch_to_cst(std::time(nullptr));
      return c.hour > 15 || (c.hour == 15 && c.minute >= 0);
    };

    // 默认循环模式：每 interval 秒一轮，收盘后连续 3 轮无新 bar 退出。
    std::signal(SIGINT, OnFetchKlineSignal);
    std::signal(SIGTERM, OnFetchKlineSignal);
    std::cerr << "=== fetch-kline 循环模式，间隔 " << interval
              << "s，15:00 后 3 轮无新 bar 退出（Ctrl-C 中断）===\n";
    int idle = 0; // 连续无新 bar 轮数
    int64_t last_max_ts = 0;
    int round_idx = 0;
    while (!g_fetch_kline_stop)
    {
      auto t0 = std::chrono::steady_clock::now();
      ++round_idx;
      auto [n, max_ts] = run_once();
      bool new_bar = (max_ts > last_max_ts);
      last_max_ts = max_ts;
      std::cerr << "[轮 " << round_idx << "] " << n << " 行(含覆盖)"
                << (new_bar ? "" : " (无新 bar)")
                << "  当日入库 " << seen_keys.size() << " 根(去重)\n";
      if (!new_bar)
      {
        ++idle;
        if (market_closed() && idle >= 3)
        {
          std::cerr << "收盘后连续 " << idle << " 轮无新 bar，退出\n";
          break;
        }
      }
      else
      {
        idle = 0;
      }
      if (g_fetch_kline_stop)
        break;

      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
      int64_t wait = interval - elapsed;
      if (wait > 0)
      {
        for (int64_t w = 0; w < wait && !g_fetch_kline_stop; ++w)
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
    std::cout << "=== fetch-kline 循环结束，当日共入库 " << seen_keys.size() << " 根(去重) ===\n";
    return 0;
  }

  // 从网络拉取日线写入 TDengine（本地 vipdoc 无 .day 文件的代码补导）。
  // pull-kline 已移除；历史 K 线由 tdx import（vipdoc）+ tdx fetch-kline（当日循环）覆盖。

  // ---- 财务 0x10：清库后从网络重导，入库 finance 全 34 列 ----
  int DoFetchFinance(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "用法: tdx fetch-finance <sh|sz|bj><code> [code...]\n";
      return 1;
    }
    std::vector<std::pair<Market, std::string>> targets;
    for (int i = 2; i < argc; ++i)
    {
      auto [m, c] = tdx::ParseMarketCode(argv[i]);
      if (c.empty())
      {
        std::cerr << argv[i] << ": 缺市场前缀（sh/sz/bj）\n";
        return 1;
      }
      targets.emplace_back(m, std::move(c));
    }
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "opentdx 连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;
    tdx::taos::EnsureFinanceTable(conn->native());

    int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000LL;
    int ok = 0;
    for (auto &[market, code] : targets)
    {
      auto cleared = tdx::taos::ClearFinance(conn->native(), code);
      auto f = sq.GetFinance(market, code);
      // 过滤全 0 空壳（code 不存在）。任一字段非 0 即入库（含 ETF/基金/指数）。
      if (f.code.empty() || (f.liutongguben == 0.0 && f.zongguben == 0.0 &&
                             f.ipo_date == 0 && f.industry == 0))
      {
        std::cerr << code << ": 空壳响应，跳过（已清 " << cleared << " 行）\n";
        continue;
      }
      auto n = tdx::taos::InsertFinanceFull(conn->native(), f, now_ms);
      if (n > 0)
      {
        ++ok;
        std::cout << code << ": finance 重导完成（清 " << cleared << " → 写 1 行 34 列）\n";
      }
      else
      {
        std::cerr << code << ": 写入失败\n";
      }
    }
    std::cout << "=== finance 完成，" << ok << "/" << targets.size() << " 只 ===\n";
    return 0;
  }

  // ---- F10 0x2cf/0x2d0：清库后从网络重导（目录 + 全文切片）----
  int DoFetchF10(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "用法: tdx fetch-f10 <sh|sz|bj><code> [code...]\n";
      return 1;
    }
    std::vector<std::pair<Market, std::string>> targets;
    for (int i = 2; i < argc; ++i)
    {
      auto [m, c] = tdx::ParseMarketCode(argv[i]);
      if (c.empty())
      {
        std::cerr << argv[i] << ": 缺市场前缀（sh/sz/bj）\n";
        return 1;
      }
      targets.emplace_back(m, std::move(c));
    }
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "opentdx 连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto conn = tdx::taos::ConnectTdx();
    if (!conn) return 1;
    tdx::taos::EnsureF10Tables(conn->native());

    // day_ts = 当日 00:00 CST 毫秒，全文切片 ts = day_ts + seq（重导覆盖不膨胀）。
    // ponytail: localtime_r+mktime 仅 main 线程安全，需迁移到 absl::Time+FixedTimeZone(+8)
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    int64_t now_ms = static_cast<int64_t>(now) * 1000LL;
    int64_t day_ts = static_cast<int64_t>(std::mktime(&lt)) * 1000LL;

    int ok = 0;
    for (auto &[market, code] : targets)
    {
      int64_t cleared = tdx::taos::ClearF10(conn->native(), code);
      auto cats = sq.GetF10Category(market, code);
      if (cats.empty())
      {
        std::cerr << code << ": 无 F10 目录（已清 " << cleared << " 子表）\n";
        continue;
      }
      int64_t cat_n = tdx::taos::InsertF10Cat(conn->native(), cats, code, now_ms);
      int64_t text_n = 0;
      for (size_t j = 0; j < cats.size(); ++j)
      {
        auto full = sq.GetF10FullText(market, code, cats[j]);
        text_n += tdx::taos::InsertF10Text(conn->native(), code, j, cats[j], full, day_ts);
      }
      ++ok;
      std::cout << code << ": F10 重导完成（清 " << cleared << " 子表，目录 " << cat_n
                << " + 切片 " << text_n << "，" << cats.size() << " 类）\n";
    }
    std::cout << "=== f10 完成，" << ok << "/" << targets.size() << " 只 ===\n";
    return 0;
  }

  // ---- 历史委托 0xfb4 ----
  int DoHistoryOrders(int argc, char **argv)
  {
    if (argc < 4)
    {
      std::cerr << "用法: tdx history-orders <sh|sz|bj><code> <YYYYMMDD>\n";
      return 1;
    }
    auto [market, code] = tdx::ParseMarketCode(argv[2]);
    if (code.empty())
    {
      std::cerr << "缺市场前缀（sh/sz/bj）\n";
      return 1;
    }
    uint32_t date = static_cast<uint32_t>(std::atol(argv[3]));
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto orders = sq.GetHistoryOrders(market, code, date);
    std::cout << code << " " << date << " 委托 (" << orders.size() << " 条):\n";
    for (const auto &o : orders)
      printf("  price=%.2f vol=%ld\n", o.price, (long)o.vol);
    return 0;
  }

  // ---- 历史逐笔 0xfb5 ----
  int DoHistoryTx(int argc, char **argv)
  {
    if (argc < 4)
    {
      std::cerr << "用法: tdx history-tx <sh|sz|bj><code> <YYYYMMDD>\n";
      return 1;
    }
    auto [market, code] = tdx::ParseMarketCode(argv[2]);
    if (code.empty())
    {
      std::cerr << "缺市场前缀（sh/sz/bj）\n";
      return 1;
    }
    uint32_t date = static_cast<uint32_t>(std::atol(argv[3]));
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto txns = sq.GetHistoryTransaction(market, code, date, 0, 2000);
    std::cout << code << " " << date << " 逐笔 (" << txns.size() << " 条):\n";
    for (size_t i = 0; i < std::min(txns.size(), size_t(20)); ++i)
    {
      const auto &t = txns[i];
      int h = t.minutes / 60 % 24, m = t.minutes % 60;
      printf("  %02d:%02d price=%.2f vol=%ld bs=%d\n", h, m, t.price, (long)t.vol, t.buy_sell);
    }
    if (txns.size() > 20)
      std::cout << "  ... 共 " << txns.size() << " 条\n";
    return 0;
  }

  // ---- 成交量分布 0x51a ----
  int DoVolProfile(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "用法: tdx vol-profile <sh|sz|bj><code>\n";
      return 1;
    }
    auto [market, code] = tdx::ParseMarketCode(argv[2]);
    if (code.empty())
    {
      std::cerr << "缺市场前缀（sh/sz/bj）\n";
      return 1;
    }
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto vp = sq.GetVolumeProfile(market, code);
    printf("%s 量价分布:\n", code.c_str());
    printf("  现价: %.2f O:%.2f H:%.2f L:%.2f 昨收:%.2f 量:%.0f 额:%.0f\n",
           vp.price, vp.open, vp.high, vp.low, vp.pre_close, vp.vol, vp.amount);
    for (const auto &l : vp.levels)
      printf("  P=%.2f vol=%ld buy=%ld sell=%ld\n", l.price, (long)l.vol, (long)l.buy, (long)l.sell);
    return 0;
  }

  // ---- 指数信息 0x51d ----
  int DoIndexInfo(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "用法: tdx index-info <sh|sz|bj><code>\n";
      return 1;
    }
    auto [market, code] = tdx::ParseMarketCode(argv[2]);
    if (code.empty())
    {
      std::cerr << "缺市场前缀（sh/sz/bj）\n";
      return 1;
    }
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto ii = sq.GetIndexInfo(market, code);
    printf("%s: 现价%.2f 昨收%.2f 涨跌%.2f O%.2f H%.2f L%.2f 量%.0f 额%.0f\n",
           code.c_str(), ii.close, ii.pre_close, ii.diff, ii.open, ii.high, ii.low, ii.vol, ii.amount);
    printf("  上涨%ld 下跌%ld\n", (long)ii.up_count, (long)ii.down_count);
    return 0;
  }

  // ---- 主力异动 0x563 ----
  int DoUnusual(int argc, char **argv)
  {
    int market = 1; // 默认 SH
    if (argc > 2)
    {
      std::string m = argv[2];
      if (m == "0" || m == "1" || m == "2")
        (void)absl::SimpleAtoi(m, &market);  // 已前置过滤，溢出不可能
      else
      {
        std::cerr << "用法: tdx unusual [market]  market∈{0=SZ,1=SH,2=BJ}（非股票代码）\n";
        return 1;
      }
    }
    quotes::StdQuotes sq;
    if (auto ec = sq.Connect())
    {
      std::cerr << "连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto items = sq.GetUnusual(static_cast<Market>(market), 0, 600);
    std::cout << "异动 (" << items.size() << " 条):\n";
    for (const auto &u : items)
      printf("  %s %02d:%02d:%02d %s %s\n", u.code.c_str(),
             u.hour, u.minute, u.second, u.desc.c_str(), u.value_str.c_str());
    return 0;
  }

  // ---- 板块列表 0x1231 ----
  int DoBoardList(int argc, char **argv)
  {
    int type = 1;
    if (argc > 2) (void)absl::SimpleAtoi(argv[2], &type);  // BoardType, 溢出→保留 1
    quotes::SPQuotes sp;
    if (auto ec = sp.Connect())
    {
      std::cerr << "SP连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto body = proto::serialize_sp_board_list(static_cast<BoardType>(type), 0, 150);
    auto resp = sp.Call(proto::kMsgSpBoardList, body);
    if (resp.body.empty())
    {
      std::cerr << "无数据\n";
      return 0;
    }
    auto boards = proto::deserialize_sp_board_list(resp.body.data(), resp.body.size());
    std::cout << "板块 (" << boards.size() << " 个):\n";
    for (size_t i = 0; i < std::min(boards.size(), size_t(20)); ++i)
      printf("  %s %s %.2lf\n", boards[i].code.c_str(), boards[i].name.c_str(), boards[i].price);
    if (boards.size() > 20)
      std::cout << "  ... 共 " << boards.size() << " 个\n";
    return 0;
  }

  // ---- 板块成员报价 0x122C ----
  int DoBoardQuotes(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "用法: tdx board-quotes <board_id>（板块ID，见 board-list 输出）\n";
      return 1;
    }
    std::string code = argv[2];
    if (code.find_first_not_of("0123456789") != std::string::npos)
    {
      std::cerr << "board_id 须为正整数（来自 board-list），而非股票/指数代码\n";
      return 1;
    }
    int board_code = 0;
    if (!absl::SimpleAtoi(code, &board_code) || board_code <= 0)
    {
      std::cerr << "board_id 解析失败或非正数\n";
      return 1;
    }
    quotes::SPQuotes sp;
    if (auto ec = sp.Connect())
    {
      std::cerr << "SP连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto body = proto::serialize_sp_board_members(board_code, SortType::Code, 0, 80, SortOrder::None, {});
    auto resp = sp.Call(proto::kMsgSpBoardMembers, body);
    if (resp.body.empty())
    {
      std::cerr << "无数据\n";
      return 0;
    }
    std::cout << "板块 " << code << " 成员报价请求已发送 (resp " << resp.body.size() << "B)\n";
    return 0;
  }

  // ---- 资金流向 0x1218 ----
  int DoCapitalFlow(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "用法: tdx capital-flow <sh|sz|bj><code>\n";
      return 1;
    }
    auto [market, code] = tdx::ParseMarketCode(argv[2]);
    if (code.empty())
    {
      std::cerr << "缺市场前缀（sh/sz/bj）\n";
      return 1;
    }
    quotes::SPQuotes sp;
    if (auto ec = sp.Connect())
    {
      std::cerr << "SP连接失败: " << ec.message() << "\n";
      return 1;
    }
    auto body = proto::serialize_sp_capital_flow(static_cast<uint16_t>(market), code);
    auto resp = sp.Call(proto::kMsgSpCapitalFlow, body);
    if (resp.body.empty())
    {
      std::cerr << "无数据\n";
      return 0;
    }
    auto flows = proto::deserialize_sp_capital_flow(resp.body.data(), resp.body.size());
    std::cout << code << " 资金流向:\n";
    for (const auto &cf : flows)
      printf("  主力净:%.0lf 散户净:%.0lf 5日主力:%.0lf\n", cf.main_net, cf.small_net, cf.five_day_main);
    return 0;
  }

} // namespace

// import 子命令（定义在 cli/import.cpp，不在 namespace 内）
int DoImport(int argc, char **argv, int jobs);

// fetch-quotes 子命令（定义在 cli/fetch_quotes.cpp）
int DoFetchQuotes(int argc, char **argv);

// 在 absl ParseCommandLine (由 MainInitGuard 触发) 之前拦截 --help/-h/help，
// 否则 gflags 会吃掉 --help 直接 exit(0)，用户永远看不到子命令列表。
static void PrintUsage()
{
  std::cerr << "用法:\n"
            << "  tdx server-test                  测速服务器\n"
            << "  tdx import [taos] [codes...]     本地数据→TDengine\n"
            << "  tdx fetch-quotes [--loop] [--codes ...]             实时行情采集→TDengine\n"
            << "  tdx fetch-kline <code> [code...] [periods] [count]  当日K线→TDengine（1d/5m/1m，循环刷新）\n"
            << "  tdx fetch-finance <code>         财务数据入库\n"
            << "  tdx fetch-f10 <code>             F10基本资料入库\n"
            << "  tdx fetch-names                  独立同步代码→名称对照表\n"
            << "  tdx check-names                  检查代码名称完整性\n"
            << "  tdx cleanup                      清理非A股/退市标的子表\n"
            << "  tdx truncate-quotes              清空实时行情表（DROP+重建）\n"
            << "  tdx history-orders <code> <date> 历史委托(YYYYMMDD)\n"
            << "  tdx history-tx <code> <date>     历史逐笔(YYYYMMDD)\n"
            << "  tdx vol-profile <code>           成交量分布\n"
            << "  tdx index-info <code>            指数信息\n"
            << "  tdx unusual [market=1]           主力异动\n"
            << "  tdx board-list [type=1]          板块列表\n"
            << "  tdx board-quotes <board_id>      板块成员报价（board_id 来自 board-list）\n"
            << "  tdx capital-flow <code>          资金流向\n";
}

int main(int argc, char **argv)
{
  // 在 gflags 解析前拦截 help 请求
  if (argc > 1)
  {
    std::string a1 = argv[1];
    if (a1 == "--help" || a1 == "-h" || a1 == "help")
    {
      PrintUsage();
      return 0;
    }
  }

  // helio MainInitGuard 内的 absl::ParseCommandLine 会拒绝未注册的 '--' flag（报
  // "Unknown command line flag" 直接退出）。import 子命令的 --full-reset /
  // --no-clear-intraday / --all-market / --zxg-blk 是手动解析（import.cpp::ParseArgs）
  // 的非 absl flag，故先把它们从 argv 剥离，让 absl 只解析已注册的 --jobs 等；
  // MainInitGuard 后保留原 argc/argv，交由各子命令自行解析（argv[0] 程序名不变）。
  std::vector<char *> absl_argv{argv[0]};
  for (int i = 1; i < argc; ++i)
  {
    std::string a = argv[i];
    bool manual = a == "--full-reset" || a == "--no-clear-intraday" ||
                  a == "--all-market" || a == "--zxg-blk";
    if (manual)
    {
      if (a == "--zxg-blk" && i + 1 < argc) ++i;  // 跳过它的路径值
      continue;
    }
    absl_argv.push_back(argv[i]);
  }
  int absl_argc = static_cast<int>(absl_argv.size());
  char **absl_argv_ptr = absl_argv.data();
  MainInitGuard guard(&absl_argc, &absl_argv_ptr);

  if (argc < 2)
  {
    PrintUsage();
    return 1;
  }
  std::string cmd = argv[1];
  if (cmd == "server-test")
    return DoServerTest();
  if (cmd == "import")
    return DoImport(argc, argv, static_cast<int>(absl::GetFlag(FLAGS_jobs)));
  if (cmd == "fetch-quotes")
    return DoFetchQuotes(argc, argv);
  if (cmd == "check-names")
    return DoCheckNames();
  if (cmd == "fetch-names")
    return DoFetchNames();
  if (cmd == "cleanup")
    return DoCleanup();
  if (cmd == "truncate-quotes")
    return DoTruncateQuotes();
  if (cmd == "fetch-kline")
    return DoFetchKline(argc, argv);
  if (cmd == "fetch-finance")
    return DoFetchFinance(argc, argv);
  if (cmd == "fetch-f10")
    return DoFetchF10(argc, argv);
  if (cmd == "history-orders")
    return DoHistoryOrders(argc, argv);
  if (cmd == "history-tx")
    return DoHistoryTx(argc, argv);
  if (cmd == "vol-profile")
    return DoVolProfile(argc, argv);
  if (cmd == "index-info")
    return DoIndexInfo(argc, argv);
  if (cmd == "unusual")
    return DoUnusual(argc, argv);
  if (cmd == "board-list")
    return DoBoardList(argc, argv);
  if (cmd == "board-quotes")
    return DoBoardQuotes(argc, argv);
  if (cmd == "capital-flow")
    return DoCapitalFlow(argc, argv);

  // ---- 向后兼容别名（v0.15.0 重命名，下个大版本可移除） ----
  if (cmd == "sync-names") {
    std::cerr << "[sync-names → fetch-names] 命令已重命名，换用 fetch-names\n";
    return DoFetchNames();
  }
  if (cmd == "sync-kline") {
    std::cerr << "[sync-kline → fetch-kline] 命令已重命名，换用 fetch-kline\n";
    return DoFetchKline(argc, argv);
  }
  if (cmd == "finance") {
    std::cerr << "[finance → fetch-finance] 命令已重命名，换用 fetch-finance\n";
    return DoFetchFinance(argc, argv);
  }
  if (cmd == "f10") {
    std::cerr << "[f10 → fetch-f10] 命令已重命名，换用 fetch-f10\n";
    return DoFetchF10(argc, argv);
  }
  std::cerr << "未知命令: " << cmd << "\n";
  return 1;
}
