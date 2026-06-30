// TDengine 多线程导入入口。
// 组合 VipdocReader（本地文件）+ StdQuotes（复权因子）+ TaosConnection（写入）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/taos/taos_connection.hpp"

namespace tdx::taos {

struct ImportTaosConfig {
  TaosConfig taos;
  std::string vipdoc_path = "/home/li/.local/share/tdxcfv/drive_c/tc/vipdoc";
  bool no_adjust = false;
  int  jobs = 1;  // 0 = auto (hardware_concurrency)
  std::vector<std::string> codes;
};

struct ImportResult {
  int codes_total = 0;
  int codes_ok = 0;
  int64_t kline_rows = 0;
  int adjust_events = 0;
  int stock_names = 0;
};

// 主入口：扫描 vipdoc → 多线程导入 TDengine
ImportResult DoImportTaos(const ImportTaosConfig& cfg);

// 独立同步股票代码→名称对照表（可脱离 import 单独调用）
int SyncStockNames(TAOS* conn);

// 独立清理非 A 股及退市标的子表（可脱离 import 单独调用）
int CleanupStaleCodes(TAOS* conn);

// 股票代码过滤：保留 A 股 + 指数 + ETF/LOF 基金，排除债券/B 股/港股通
bool IsAStock(const std::string& code);

// 从网络拉取日线写入 TDengine（用于本地 vipdoc 无 .day 文件的代码，如 589xxx ETF）。
// 返回成功导入行数。
struct NetworkImportResult {
  int codes_ok = 0;
  int64_t kline_rows = 0;
};
NetworkImportResult ImportKlineFromNetwork(TAOS* conn,
                                           const std::vector<std::string>& codes);

}  // namespace tdx::taos
