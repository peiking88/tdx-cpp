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
  std::string engine = "taos";  // 默认 taos
  bool no_adjust = false;
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
    if (a == "taos") cfg.engine = "taos";
    else if (a == "duckdb") {
      std::cerr << "错误: DuckDB 引擎已移除，请使用 taos。\n";
      std::exit(1);
    }
    else if (a == "help") {
      std::cout << "用法: tdx import [taos] [codes...]\n\n"
                << "  taos            存储引擎（默认）\n"
                << "  codes...        股票代码（默认扫描 vipdoc 全部）\n\n"
                << "环境变量:\n"
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
  tcfg.taos        = cfg.taos;
  tcfg.vipdoc_path = cfg.vipdoc_path;
  tcfg.no_adjust   = cfg.no_adjust;
  tcfg.jobs        = jobs;
  tcfg.codes       = cfg.codes;
  auto result = tdx::taos::DoImportTaos(tcfg);
  return (result.kline_rows >= 0) ? 0 : 1;
}
