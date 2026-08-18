// data_loader 实现：TDengine 历史日线 + mmap 当日快照（tdx::shm）拼接。
//
// 连接统一走 tdx::taos::TaosConnection（tdx_taos_conn，RAII 叶子，零 helio 依赖）；
// 查询仍用 libtaos C API（taos_query/taos_fetch_row），资源由 RAII/局部释放管理。
#include "czsc/io/data_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>

#include <taos.h>

#include "tdx/shm/segment.hpp"        // Segment::OpenReadOnly / SnapshotTable
#include "tdx/shm/payload.hpp"        // QuotePOD
#include "tdx/taos/taos_connection.hpp"  // TaosConnection RAII（统一连接管理）

namespace czsc::io {

namespace {

// 环境变量取值，未设置返回 default。
std::string EnvOr(const char* name, std::string def) {
  if (const char* v = std::getenv(name)) return v;
  return def;
}

// 统一连接入口：LoaderConfig → tdx::taos::TaosConnection（RAII）。
// DB 不可达返回 nullopt，调用方按空数据处理。
std::optional<tdx::taos::TaosConnection> OpenTaos(const LoaderConfig& cfg) {
  tdx::taos::TaosConfig tc;
  tc.host = cfg.taos_host;
  tc.port = cfg.taos_port;
  tc.user = cfg.taos_user;
  tc.pass = cfg.taos_pass;
  tc.db = cfg.taos_db;
  tdx::taos::TaosConnection conn(tc);
  if (!conn) return std::nullopt;
  return conn;
}

// RawBar.symbol 后缀（大写 .SH/.SZ/.BJ/.HK），由 mkt 决定。
// ponytail: 仅供 symbol 标注，分析层不依赖后缀正确性。
std::string InferSymbol(const std::string& code, Market mkt) {
  if (code.empty()) return code;
  const char* suffix = (mkt == Market::kSh) ? "SH" : (mkt == Market::kSz) ? "SZ"
                     : (mkt == Market::kBj) ? "BJ" : "HK";
  return code + "." + suffix;
}

// 历史日线：SELECT 子表 k_<mkt><code>_1d，按 ts 升序。失败/空返回空。
//   mkt: 显式市场（决定子表前缀）。
std::vector<RawBar> LoadDailyFromTaos(const std::string& code, const LoaderConfig& cfg,
                                      Market mkt) {
  if (mkt == Market::kHk) return {};  // 港股不入 TDengine（仅 mmap 当日快照，不做缠论分析）
  auto conn = OpenTaos(cfg);
  if (!conn) return {};  // DB 不可达：调用方按空处理
  TAOS* c = conn->native();
  // 子表名 k_<mkt><code>_1d（tdx-cpp 写入约定，含小写市场前缀）。
  std::string sql = "SELECT ts,open,high,low,close,volume,amount FROM k_" +
                    std::string(MarketToPrefix(mkt)) + code + "_1d ORDER BY ts ASC";
  TAOS_RES* res = ::taos_query(c, sql.c_str());
  std::vector<RawBar> bars;
  if (::taos_errno(res) != 0) {  // 表不存在 / SQL 错误
    ::taos_free_result(res);
    return {};
  }
  TAOS_ROW row;
  const std::string symbol = InferSymbol(code, mkt);
  while ((row = ::taos_fetch_row(res))) {
    RawBar b;
    b.symbol = symbol;
    b.freq = Freq::kDay;
    // ts: TIMESTAMP 毫秒 → epoch 秒；OHLCV 字段可能为 NULL（缺失日）。
    if (row[0]) b.dt    = *static_cast<const int64_t*>(row[0]) / 1000;
    if (row[1]) b.open  = *static_cast<const double*>(row[1]);
    if (row[4]) b.close = *static_cast<const double*>(row[4]);
    if (row[2]) b.high  = *static_cast<const double*>(row[2]);
    if (row[3]) b.low   = *static_cast<const double*>(row[3]);
    if (row[5]) b.vol   = *static_cast<const double*>(row[5]);
    if (row[6]) b.amount= *static_cast<const double*>(row[6]);
    if (b.dt > 0 && b.high >= b.low) bars.push_back(std::move(b));
  }
  ::taos_free_result(res);
  return bars;
}

// 当日成形日线：mmap QuotePOD 快照 → 一根 RawBar。不可读返回 std::nullopt。
// ponytail: dt 取该快照 UTC 日 15:00 CST（07:00 UTC）时刻，与历史日线锚点一致；
//           volume/amount 单位与 .day 源历史一致（手），mmap 同字段同源（fetch-quotes 直写）。
std::optional<RawBar> LoadTodayFromShm(const std::string& code, const LoaderConfig& cfg, Market mkt) {
  auto seg = tdx::shm::Segment::OpenReadOnly(cfg.shm_path);
  if (!seg) return std::nullopt;  // shm 未创建（fetch-quotes 未启动）
  tdx::shm::QuotePOD q{};
  if (!seg->Snapshot().Get(code, q)) return std::nullopt;  // 该 code 无快照
  if (q.datetime <= 0 || q.high <= 0) return std::nullopt;

  RawBar b;
  b.symbol = InferSymbol(code, mkt);  // A 股 + 港股快照均写 mmap（fetch-quotes --quote_hk）
  b.freq = Freq::kDay;
  b.dt    = (q.datetime / 86400) * 86400 + 7 * 3600;  // 当日 15:00 CST
  b.open  = q.open;
  b.high  = q.high;
  b.low   = (q.low > 0) ? q.low : std::min(q.open, q.price);
  b.close = q.price;       // 现价即最新收盘
  b.vol   = q.volume;
  b.amount= q.amount;
  return b;
}

}  // namespace

LoaderConfig LoaderConfig::FromEnv() {
  LoaderConfig c;
  c.taos_host = EnvOr("CZSC_TAOS_HOST", c.taos_host);
  c.taos_user = EnvOr("CZSC_TAOS_USER", c.taos_user);
  c.taos_pass = EnvOr("CZSC_TAOS_PASS", c.taos_pass);
  c.taos_db   = EnvOr("CZSC_TAOS_DB",   c.taos_db);
  if (const char* p = std::getenv("CZSC_TAOS_PORT")) {
    int port = 0;
    auto [ptr, ec] = std::from_chars(p, p + std::strlen(p), port);
    if (ec == std::errc{} && port > 0 && port <= 65535) {
      c.taos_port = static_cast<uint16_t>(port);
    } else {
      std::fprintf(stderr, "[data_loader] 忽略无效端口 %s，使用默认 %u\n", p, c.taos_port);
    }
  }
  c.shm_path  = EnvOr("CZSC_SHM_PATH",  c.shm_path);
  return c;
}

std::vector<RawBar> LoadDailyBars(const std::string& code, const LoaderConfig& cfg,
                                  Market mkt) {
  auto bars = LoadDailyFromTaos(code, cfg, mkt);
  auto today = LoadTodayFromShm(code, cfg, mkt);
  if (today) {
    const int64_t day = today->dt / 86400;
    // 同交易日历史 bar（盘后已入库）用实时快照覆盖，避免重复。
    bars.erase(std::remove_if(bars.begin(), bars.end(),
                              [day](const RawBar& b) { return b.dt / 86400 == day; }),
               bars.end());
    bars.push_back(*today);
    std::sort(bars.begin(), bars.end(),
              [](const RawBar& a, const RawBar& b) { return a.dt < b.dt; });
  }
  return bars;
}

// 5m K 线加载：SELECT 子表 k_<mkt><code>_5m（tdx-cpp 同步写入，vipdoc fzline/lc5）。
//   mkt: 显式市场（决定子表前缀）。
std::vector<RawBar> Load5mBars(const std::string& code, const LoaderConfig& cfg,
                               Market mkt) {
  if (mkt == Market::kHk) return {};  // 港股不入 TDengine
  auto conn = OpenTaos(cfg);
  if (!conn) return {};
  TAOS* c = conn->native();
  std::string sql = "SELECT ts,open,high,low,close,volume,amount FROM k_" +
                    std::string(MarketToPrefix(mkt)) + code + "_5m ORDER BY ts ASC";
  TAOS_RES* res = ::taos_query(c, sql.c_str());
  std::vector<RawBar> bars;
  if (::taos_errno(res) != 0) {
    ::taos_free_result(res);
    return {};
  }
  TAOS_ROW row;
  const std::string symbol = InferSymbol(code, mkt);
  while ((row = ::taos_fetch_row(res))) {
    RawBar b;
    b.symbol = symbol;
    b.freq = Freq::k5Min;
    if (row[0]) b.dt    = *static_cast<const int64_t*>(row[0]) / 1000;
    if (row[1]) b.open  = *static_cast<const double*>(row[1]);
    if (row[4]) b.close = *static_cast<const double*>(row[4]);
    if (row[2]) b.high  = *static_cast<const double*>(row[2]);
    if (row[3]) b.low   = *static_cast<const double*>(row[3]);
    if (row[5]) b.vol   = *static_cast<const double*>(row[5]);
    if (row[6]) b.amount= *static_cast<const double*>(row[6]);
    if (b.dt > 0 && b.high >= b.low && b.close > 0) bars.push_back(std::move(b));
  }
  ::taos_free_result(res);
  return bars;
}

// 5m → 30m 重采样。桶按自然时间 30 分钟对齐（以当日 00:00 CST 为基准）。
// k5Min bars 沿交易时段 09:30~15:00，桶边界 [09:00,09:30,...]，30 分钟含 6 根 5m bar。
std::vector<RawBar> Resample5mTo30m(const std::vector<RawBar>& bars_5m) {
  std::vector<RawBar> out;
  if (bars_5m.empty()) return out;
  const int64_t kDaySec = 86400;
  const int64_t k30Min = 1800;
  // 桶键：当日 00:00 起算的 30 分钟序号。跨日桶键跳变，自然分隔。
  int64_t cur_key = -1;
  RawBar agg;
  bool has = false;
  for (const auto& b : bars_5m) {
    int64_t day = b.dt / kDaySec;
    int64_t off = b.dt - day * kDaySec;           // 当日零点偏移（UTC 基准，CST=UTC+8）
    int64_t bucket = off / k30Min;
    int64_t key = day * (kDaySec / k30Min) + bucket;
    if (key != cur_key) {
      if (has) out.push_back(std::move(agg));
      agg = b;
      agg.freq = Freq::k30Min;
      agg.dt = day * kDaySec + bucket * k30Min;   // 桶起始时刻
      has = true;
      cur_key = key;
    } else {
      agg.high   = std::max(agg.high, b.high);
      agg.low    = std::min(agg.low, b.low);
      agg.close  = b.close;
      agg.vol   += b.vol;
      agg.amount+= b.amount;
    }
  }
  if (has) out.push_back(std::move(agg));
  return out;
}

// 日线 → 周线：按日历年/ISO 周聚合。桶键 = 周内周一 15:00 CST（07:00 UTC）。
std::vector<RawBar> ResampleDailyToWeek(const std::vector<RawBar>& bars_daily) {
  std::vector<RawBar> out;
  if (bars_daily.empty()) return out;
  const int64_t kDaySec = 86400;
  // 锚定到含 1970-01-01（周四）的基准周一。周一 = UNIX 周一；date_to_week_start。
  auto WeekStart = [](int64_t dt) -> int64_t {
    // dt 应锚定 15:00 CST = 07:00 UTC 当日。试算：1970-01-01 07:00 UTC 即 dt 当天，
    // 当天是周四，周一起点 = dt - 3 天（按 7 天周期折回）。
    int64_t day = dt / 86400;
    int64_t weekday = ((day % 7) + 7) % 7;   // 1970-01-01=周四 → day%7=0 → 0=Thu
    // Thu=0, Fri=1, Sat=2, Sun=3, Mon=4, Tue=5, Wed=6
    int64_t back = (weekday - 4 + 7) % 7;    // 周一对应 weekday=4，需回退 back 天
    // 周一 15:00 CST
    return (day - back) * 86400 + 7 * 3600;
  };
  int64_t cur_week = -1;
  RawBar agg;
  bool has = false;
  for (const auto& b : bars_daily) {
    int64_t ws = WeekStart(b.dt);
    if (ws != cur_week) {
      if (has) out.push_back(std::move(agg));
      agg = b;
      agg.freq = Freq::kWeek;
      agg.dt = ws;
      has = true;
      cur_week = ws;
    } else {
      agg.high   = std::max(agg.high, b.high);
      agg.low    = std::min(agg.low, b.low);
      agg.close  = b.close;
      agg.vol   += b.vol;
      agg.amount+= b.amount;
    }
  }
  if (has) out.push_back(std::move(agg));
  return out;
}

}  // namespace czsc::io
