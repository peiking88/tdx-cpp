// 数据加载层（Phase 6 IO）：TDengine 历史 K 线 + mmap 实时行情拼接。
//
// 参考 ~/peiking88/tdx-cpp/tools/mmap_viewer.cpp 的快照读取方式：
// 历史日线来自 TDengine stable `kline`（子表 k_<code>_1d），
// 当日成形日线来自 /dev/shm/tdx_quotes.shm 的 QuotePOD 快照（盘中实时刷新）。
// ponytail: mmap 的 1m/5m ring 区在 tdx-cpp 中尚未填充（MVP=0），
//           故 mmap 当前仅能给出「当日日线聚合」，本层只拼接日线。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "czsc/types/market.hpp"
#include "czsc/types/raw_bar.hpp"

namespace czsc::io {

// 加载配置。所有字段均可由环境变量覆盖（FromEnv）。
struct LoaderConfig {
  std::string taos_host = "localhost";
  uint16_t    taos_port = 6030;
  std::string taos_user = "root";
  std::string taos_pass = "taosdata";
  std::string taos_db   = "tdx";
  std::string shm_path  = "/dev/shm/tdx_quotes.shm";

  // 环境变量覆盖（未设置则保留默认）：CZSC_TAOS_HOST/PORT/USER/PASS/DB、CZSC_SHM_PATH。
  static LoaderConfig FromEnv();
};

// 加载某标的日线：TDengine 历史 + mmap 实时拼接当日 K 线。
//   code: 裸码（无前缀），如 "002515"；mkt: 显式市场（决定子表前缀，不再靠 code 推断）。
//   A 股：子表 k_<mkt><code>_1d（如 k_sz002515_1d）+ mmap 当日拼接。
//   港股(kHk)：不入 TDengine，仅返回 mmap 当日快照（fetch-quotes --quote_hk 写入）；
//              无历史序列、不做缠论分析，仅供 czsc_data_viewer 看盘。
//   返回值：升序日线 RawBar[]；DB 不可达或子表不存在时返回空。
//
// 拼接语义（按交易日去重）：
//   - 若历史末尾已是当日（盘后已入库）→ 用 mmap 实时快照覆盖该 bar；
//   - 否则（盘中，DB 当日尚无）→ 追加一个由快照构造的当日 bar；
//   - mmap 不可读（非交易时段 / fetch-quotes 未启动）→ 仅返回历史。
std::vector<RawBar> LoadDailyBars(const std::string& code, const LoaderConfig& cfg,
                                  Market mkt);

// 加载某标的 5 分钟 K 线：TDengine 子表 k_<mkt><code>_5m，按 ts 升序。
//   mkt: 显式市场，同 LoadDailyBars。港股(kHk)不入库，直接返回空。
//   返回值：升序 5m RawBar[]；DB 不可达或子表不存在时返回空。
// ponytail: 5m 子表由 tdx-cpp 同步写入（vipdoc fzline/lc5），本层仅 SELECT。
std::vector<RawBar> Load5mBars(const std::string& code, const LoaderConfig& cfg,
                               Market mkt);

// 重采样：5m → 30m。按自然时间 30 分钟桶聚合（09:30/10:00/.../14:30 共 8 桶/日），
// 跨日不混桶。输入须升序。输出 freq=k30Min，dt 为桶起始时刻（CST 锚点）。
// ponytail: 简单桶聚合，不处理集合竞价/盘前；与 czsc-python resample_to_30m 语义对齐。
std::vector<RawBar> Resample5mTo30m(const std::vector<RawBar>& bars_5m);

// 重采样：日线 → 周线。按自然周（周一~周日）聚合，跨周年不混桶。输入须升序。
// 输出 freq=kWeek，dt 为每周一 15:00 CST 锚点。
std::vector<RawBar> ResampleDailyToWeek(const std::vector<RawBar>& bars_daily);

}  // namespace czsc::io
