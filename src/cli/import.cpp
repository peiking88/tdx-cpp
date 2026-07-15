// tdx import：TDX 本地历史数据 → TDengine 导入工具（多线程，自动判断增量/全量）。
//
// 用法：
//   tdx import [taos] [codes...]
//   环境变量:
//     TDX_HOME         vipdoc 路径
//     TDX_NO_ADJUST=1  跳过复权因子
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "tdx/taos/taos_import.hpp"

namespace {

struct ImportConfig {
  std::string vipdoc_path = "";  // ponytail: 默认空，由 TDX_HOME 环境变量设定
  std::string zxg_blk = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk";
  std::string engine = "taos";  // 默认 taos
  bool no_adjust = false;
  bool clear_intraday = true;  // 清当日盘中数据（增量留历史）
  bool full_reset = false;     // 首次迁移：DROP 整表全清
  bool all_market = false;     // 导入全市场（默认仅导入自选股 zxg.blk）
  bool daily_only = false;     // 只导入日 K 线（1d），跳过 1m/5m + 复权因子
  bool kronos = false;         // 仅个股+大盘指数（排除 ETF/LOF/板块指数）
  std::vector<std::string> codes;
  tdx::taos::TaosConfig taos = tdx::taos::TaosConfig::FromEnv();
};

ImportConfig ParseArgs(int argc, char** argv) {
  ImportConfig cfg;
  const char* home = std::getenv("TDX_HOME");
  if (home) cfg.vipdoc_path = home;
  if (std::getenv("TDX_NO_ADJUST")) cfg.no_adjust = true;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--jobs") { ++i; continue; }
    if (a.rfind("--jobs=", 0) == 0) continue;
    if (a == "--no-clear-intraday") { cfg.clear_intraday = false; continue; }
    if (a == "--full-reset") { cfg.full_reset = true; cfg.clear_intraday = false; continue; }
    if (a == "--all-market") { cfg.all_market = true; continue; }
    if (a == "--daily-only") { cfg.daily_only = true; continue; }
    if (a == "--kronos") { cfg.kronos = true; continue; }
    if (a == "--zxg-blk") { if (i + 1 < argc) cfg.zxg_blk = argv[++i]; continue; }
    if (a == "taos") cfg.engine = "taos";
    else if (a == "duckdb") {
      std::cerr << "错误: DuckDB 引擎已移除，请使用 taos。\n";
      std::exit(1);
    }
    else if (a == "help") {
      std::cout << "用法: tdx import [taos] [codes...]\n\n"
                << "  taos            存储引擎（默认）\n"
                << "  codes...        股票代码（默认仅导入自选股 zxg.blk）\n"
                << "  --all-market    导入全市场（默认仅导入自选股）\n"
                << "  --zxg-blk PATH  自选股文件路径\n"
                << "  --full-reset    首次迁移：DROP 整表全清后 vipdoc 全量重建\n"
                << "  --daily-only    只导入日 K 线（1d），跳过 1m/5m（复权因子由 --no-adjust 控制）\n"
                << "  --kronos        仅导入个股+大盘指数（排除 ETF/LOF/板块指数）；常与 --daily-only 联用\n"
                << "  --no-clear-intraday  不清当日（保留 fetch-kline 写的盘中数据）\n\n"
                << "说明: 历史增量从 vipdoc 导入，默认只清当日盘中（fetch-kline 循环刷新）。"
                << "首次迁移历史脏数据用 --full-reset。"
                << "默认仅导自选股（与 fetch-quotes 一致），--all-market 导全市场。\n\n"
                << "环境变量:\n"
                << "  TDX_ZXG_BLK      自选股文件路径（覆盖 --zxg-blk）\n\n"
                << "其他环境变量:\n"
                << "  TDX_HOME         vipdoc 路径（当前: " << cfg.vipdoc_path << "）\n"
                << "  TDX_NO_ADJUST=1  跳过复权因子\n"
                << "  TDX_TAOS_HOST    TDengine 主机（默认 localhost）\n"
                << "  TDX_TAOS_PORT    TDengine 端口（默认 6030）\n"
                << "  TDX_TAOS_USER    TDengine 用户（默认 root）\n"
                << "  TDX_TAOS_PASS    TDengine 密码（默认 taosdata）\n"
                << "  TDX_TAOS_DB      数据库名（默认 tdx）\n\n"
                << "示例:\n"
                << "  tdx import                     增量导入（自动判断）\n"
                << "  tdx import taos 600000         导入单只股票\n"
                << "  tdx import taos 600000 -n 4    4 线程并发导入\n";
      std::exit(0);
    } else {
      cfg.codes.push_back(a);
    }
  }
  return cfg;
}

}  // namespace

int DoImport(int argc, char** argv, int jobs) {
  auto cfg = ParseArgs(argc, argv);

  if (cfg.engine == "duckdb") {
    std::cerr << "错误: DuckDB 引擎已移除，请使用 taos。\n";
    return 1;
  }

  tdx::taos::ImportTaosConfig tcfg;
  tcfg.taos            = cfg.taos;
  tcfg.vipdoc_path     = cfg.vipdoc_path;
  tcfg.zxg_blk         = cfg.zxg_blk;
  tcfg.no_adjust       = cfg.no_adjust;
  tcfg.clear_intraday  = cfg.clear_intraday;
  tcfg.full_reset      = cfg.full_reset;
  tcfg.all_market      = cfg.all_market;
  tcfg.daily_only      = cfg.daily_only;
  tcfg.kronos          = cfg.kronos;
  tcfg.jobs            = jobs;
  tcfg.codes           = cfg.codes;
  auto result = tdx::taos::DoImportTaos(tcfg);
  return (result.kline_rows >= 0) ? 0 : 1;
}
