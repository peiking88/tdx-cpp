// TDengine 多线程导入入口。
// 组合 VipdocReader（本地文件）+ StdQuotes（复权因子）+ TaosConnection（写入）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdx/consts.hpp"
#include "tdx/taos/taos_connection.hpp"
#include "tdx/types.hpp"

namespace tdx::taos {

struct ImportTaosConfig {
  TaosConfig taos;
  std::string vipdoc_path = "/home/li/.local/share/tdxcfv/drive_c/tc/vipdoc";
  std::string zxg_blk = "/home/li/.local/share/tdxcfv/drive_c/tc/T0002/blocknew/zxg.blk";
  bool no_adjust = false;
  bool clear_intraday = true;  // 清当日 1d/1m/5m 盘中数据（增量留历史）
  bool full_reset = false;     // 首次迁移：DROP 整表全清，vipdoc 全量重建
  bool all_market = false;     // 导入全市场（默认仅导入自选股 zxg.blk）
  bool daily_only = false;     // 只导入日 K 线（1d），跳过 1m/5m + 复权因子
  bool kronos = false;         // 仅个股+大盘指数（排除 ETF/LOF/板块指数）
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

// 独立同步股票代码→名称对照表（可脱离 import 单独调用）。
// kronos 模式仅保留个股+大盘指数，排除 ETF/LOF/板块指数。
int SyncStockNames(TAOS* conn, bool kronos = false);

// 独立清理非 A 股及退市标的子表（可脱离 import 单独调用）
int CleanupStaleCodes(TAOS* conn);

// 股票代码过滤：保留 A 股 + 指数 + ETF/LOF 基金，排除债券/B 股/港股通
bool IsAStock(const std::string& code);

// 仅个股 + 大盘指数（排除 ETF/LOF/板块指数），供 --kronos 使用。
bool IsKronosTarget(const std::string& code);

// 从网络拉取日线写入 TDengine（用于本地 vipdoc 无 .day 文件的代码，如 589xxx ETF）。
// 返回成功导入行数。
struct NetworkImportResult {
  int codes_ok = 0;
  int64_t kline_rows = 0;
  int adj_ok = 0;
  int64_t adj_events = 0;
};
NetworkImportResult ImportKlineFromNetwork(TAOS* conn,
                                           const std::vector<std::string>& codes);

// 实时 K 线增量落库：>= last_ts 过滤（支持当日 bar 盘中更新后覆盖写入）。
// 返回实际写入行数；DB 错误返回 -1。调用前需 USE tdx + 确保 kline 超级表存在。
int64_t UpsertBars(TAOS* conn, Market market, const std::string& code,
                   const std::string& cycle, const std::vector<KLine>& bars);

// ==================== kline 超级表与公用写入助手 ====================

// 建 kline + adjust 超级表（IF NOT EXISTS 安全幂等）。
// 调用前需 USE tdx。DoImportTaos / ImportKlineFromNetwork / DoFetchKline 均依赖此函数。
void EnsureKlineTables(TAOS* conn);

// 单周期 K 线增量写入：LastTimestamp 过滤 → InsertKlineBatch。返回写入行数。
// bars 参数为非 const 拷贝（调用方可 std::move 入参），调用前需 USE tdx。
int64_t WriteKlineToDB(TAOS* conn, std::vector<KLine> bars,
                       Market market, const std::string& code, const char* tag);

// 复权因子增量写入：LastTimestamp 过滤 → 逐条 INSERT。返回写入条数。
// 调用前需 USE tdx + 确保 adjust 超级表存在。
int64_t WriteAdjustToDB(TAOS* conn, const std::vector<tdx::Xdxr>& xdxr,
                        Market market, const std::string& code);

// ==================== f10 / finance 独立导入（从 fetch-quotes 分离）====================
// finance 全列（34 业务字段：股本结构/每股/资产负债/损益/现金流/属性）。
// 旧库（8 列）通过 ALTER STABLE ADD COLUMN 补齐 26 个缺失列，历史数据 NULL 不丢。
bool EnsureFinanceTable(TAOS* conn);
int64_t ClearFinance(TAOS* conn, std::string_view code);           // DROP fn_<code>
int64_t InsertFinanceFull(TAOS* conn, const Finance& f, int64_t now_ms);  // 写全 34 列

// f10 目录 + 全文切片（单分类可达 76KB，按 1000 字符切片，seq 为片序）。
bool EnsureF10Tables(TAOS* conn);
int64_t ClearF10(TAOS* conn, std::string_view code);               // DROP fc|ft_<code>_*
int64_t InsertF10Cat(TAOS* conn, const std::vector<F10Category>& cats,
                     std::string_view code, int64_t now_ms);
int64_t InsertF10Text(TAOS* conn, const std::string& code, size_t j,
                      const F10Category& cat, const std::string& full, int64_t day_ts);

}  // namespace tdx::taos
