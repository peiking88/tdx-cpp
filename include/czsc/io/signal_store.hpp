// 信号持久化层：分析结果的 TDengine 读写。
//
// 设计：
//   - 稳定子表 signals（ts TIMESTAMP, symbol VARCHAR(16), sig VARCHAR(255)），
//     TAGS (code VARCHAR(10), market VARCHAR(2), freq VARCHAR(8))。
//   - sig 列存储 "k1_k2_k3_v1_v2_v3_score" 7 段字符串。
//   - 子表名 s_<mkt><code>_<freqTag>（如 s_sz002515_F5），周用 "week" 标签。
//   - 覆盖式写入：DROP 子表 + 全量 INSERT。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <taos.h>

#include "czsc/io/data_loader.hpp"
#include "czsc/types/enums.hpp"
#include "czsc/types/market.hpp"
#include "czsc/types/signal.hpp"

namespace czsc::io {

// freq 标签（小写）：5m→F5、30m→F30、1d→D、W→week
std::string FreqTag(Freq f);

// 规范化后的标的代码。
//   code: 裸码（无前缀），如 "00700"/"600519"；mkt: 2 位小写前缀 "sh/sz/bj/hk"；
//   display: hk00700 / sh600519 展示串。
struct CodeKey {
  std::string code;
  std::string mkt;
  std::string display;
};

// 标的代码规范化：必须 2 位前缀 + 代码（sh/sz/bj/hk），不接受裸码。
//   例："sh600519"→sh600519；"hk00700"→hk00700。
//   裸数字（A 股 6 位/港股 5 位首字符重叠，无法区分）视为非法，返回 nullopt。
std::optional<CodeKey> NormalizeCode(std::string_view raw);

// 确保超级表 signals 存在（IF NOT EXISTS 幂等）。调用方须先 USE tdx。
void EnsureSignalTable(TAOS* conn);

// 对单标的单周期，覆盖式写入信号：DROP 子表 → 批量 INSERT。
//   code: 裸 6 位；mkt: 2 位小写前缀 "sh/sz/bj"；freq: 周期标签 "F5/F30/D/week"。
//   written: 实际插入行数；出错返回 -1 并打印 stderr。调用方需已 conn USE tdx。
// ts_ms: 信号对应 K 时刻（毫秒），通常取 CZSC.bars_raw.back().dt * 1000。
int64_t RewriteSignals(TAOS* conn, const std::string& code,
                       const std::string& mkt, const std::string& freq_tag,
                       const std::vector<czsc::Signal>& signals, int64_t ts_ms);

// 读取指定标的+周期的所有信号。失败/无数据返回空。
struct SignalRow {
  int64_t ts_ms = 0;
  std::string symbol;
  std::string sig;
};
std::vector<SignalRow> QuerySignals(TAOS* conn, const std::string& code,
                                    const std::string& mkt, const std::string& freq_tag);

// 列出 signals 子表中存在的所有 (code, mkt)。用于全量扫描。
std::vector<std::pair<std::string, std::string>> ListSignalCodes(TAOS* conn);

// 转义 SQL 单引号
std::string EscapeSql(std::string_view s);

}  // namespace czsc::io
