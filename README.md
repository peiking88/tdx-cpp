# tdx-cpp

通达信行情数据 C++17 单库——TCP 协议帧编解码、A股/期货/港美股行情解析（K线/分时/逐笔/五档）、本地 vipdoc 读取、复权因子自算、TDengine 存储导入、断点续传。

## 快速开始

```bash
bash scripts/setup_external.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

## CLI 命令

> **所有 code 必须带市场前缀 `sh`/`sz`/`bj`**（如 `sh000001`=上证指数、`sz000001`=平安银行），消除歧义 code 的市场错判（v0.13.8 起强制，移除 MarketFromCode 前缀推断）。

| 命令 | 说明 |
|---|---|
| `tdx import --tdx-root <path>` | 本地 vipdoc 导入 TDengine（码表驱动 + 网络回退） |
| `tdx fetch-kline <code> [code...] [periods] [count]` | 当日 K 线循环刷新入库（1d/5m/1m，默认循环） |
| `tdx batch-fetch --stock-list <file> --start <date> --end <date> -n <N>` | 并发批量拉取 + 断点续传 |
| `tdx fetch-names` | 同步股票代码→名称对照表 |
| `tdx check-names` | 检查名称表覆盖完整性 |
| `tdx cleanup` | 清理对照表中已失效的冗余条目 |
| `tdx fetch-quotes [--loop] [--quote_interval N] [--quote_jobs N] [--quote_codes ...] [--all_market] [--zxg_blk PATH] [--mmap_path PATH] [--with_finance] [--with_f10] ...` | 实时行情采集入库。**默认采集自选股 `zxg.blk`**（入库与 mmap 共享同一范围）；`--all_market` 全市场；`--quote_codes` 显式指定优先；`--mmap_path` 启用「写 mmap 快照 + 异步入库」，空则同步入库 |
| `tdx_quotes_reader <mmap_path> <code> [interval_ms]` | 只读挂载共享内存，轮询打印某股票最新报价（供 Krono/czSC 等分析进程参考） |
| `tdx truncate-quotes` | 清空实时行情表 |
| `tdx fetch-finance <code>` | 财务数据入库（流通股本/总股本/每股收益等） |
| `tdx fetch-f10 <code>` | F10 基本资料入库 |
| `tdx history-orders <code> <YYYYMMDD>` | 历史委托队列 |
| `tdx history-tx <code> <YYYYMMDD>` | 历史逐笔成交 |
| `tdx vol-profile <code>` | 盘中成交量分布 |
| `tdx index-info <code>` | 指数信息（涨跌家数/成分股订单） |
| `tdx unusual [market=1]` | 主力异动监控 |
| `tdx board-list [type=1]` | SP/MAC 板块列表 |
| `tdx board-quotes <board_code>` | 板块成员报价 |
| `tdx capital-flow <code>` | 资金流向（主力/散户净额） |
| `tdx server-test` | 服务器测速选服 |

## 导入覆盖

| 类型 | 代码段 | 状态 |
|---|---|---|
| A 股（沪深京） | `0/3/6/4/8xxxxx` | ✅ |
| 板块/沪深指数 | `88xxxx` / `399xxx` | ✅ |
| ETF/LOF 基金 | `5xxxxx` / `159xxx` / `16xxxx` | ✅ |
| 债券 | `1xxxxx`(非159/16)、`2xxxxx` | ❌ 排除 |
| B 股 | `2/9xxxxx` | ❌ 排除 |
| 港股通 | `7xxxxx` | ❌ 排除 |

## 技术栈

C++17 / CMake + Ninja / helio (io_uring+fiber) / TDengine / Boost.Context / OpenSSL / zlib / iconv / absl::Time

## 版本

当前 `0.15.1`。版本号位于 `CMakeLists.txt` 的 `project(tdx-cpp VERSION x.y.z)`。

### 2026-07-10 v0.15.1

- **心跳 dispatcher 崩溃修复（blocker）**：`Heartbeat::OnTimer` 由 `AddPeriodic` 注册、跑在 helio periodic dispatcher fiber 上，该 fiber 不允许 `Suspend`；而 `SendHeartbeat`→`conn_->Call` 会 Suspend，落在了 dispatcher 上触发 `Should not preempt dispatcher` abort（fetch-kline 跑不过一个心跳周期 ~30s）。修复：`OnTimer` 把 `send_fn`/`timeout_fn` 包 `::util::MakeFiber().Detach()` 切到普通 fiber 执行，惠及 StdQuotes + SPQuotes。
- **fetch-kline 循环逻辑改进**：①日志去重计数——`std::set<code|tag|ts>` 跨轮累积，显示「当日入库 N 根(去重)」取代误导性的累计 INSERT 行数（TDengine 主键 upsert 幂等，旧 `total` 严重高估）；②非交易日早退——`Calendar::IsTradingDay` 判今日，周末/节假日直接退出不空转到 15:00；③修正「积重复行」误导注释；④vsrc 枚举借用关系 `ponytail:` 注释。

### 2026-07-09 v0.14.5

- **历史 K 线数据流重构**：`tdx import` 默认从 vipdoc 导入历史 1d/1m/5m（默认仅导自选股 `zxg.blk`，`--all-market` 全市场，`--full-reset` 首次迁移全清；增量留历史只清当日）；当日盘中由 `tdx fetch-kline` 默认循环刷新（60s 间隔，15:00 后 3 轮无新 bar 退出）。`import` 并发默认 4 线程。
- **协议修复**：`deserialize_finance` 偏移错位——对齐 tdxpy 真实布局 `<fHHII+30f>`（含 `zhigonggu` 职工股，旧 opentdx 误标 `meigushouyi` 且少 1 字段导致后续全错位）；股本/资产/负债/收入/利润 ×10000 缩放（万股→股、万元→元），实测 600000 与 mootdx 完全一致。
- **fetch-finance/fetch-f10 分离**：从 `fetch-quotes` 完全移除，独立为 `tdx fetch-f10` / `tdx fetch-finance` 命令（清库后从网络重导，finance 全 30 列；f10 目录+全文切片）。
- **脏数据拦截**：`deserialize_kline` D7 交易时段校验，分钟 bar 须在 9:30–11:30 或 13:00–15:00，否则丢弃（解析器 D6 错位产生的 23:55/16:39 等非交易时段 bar 在 parser 层拦截）。

### 2026-07-08 v0.14.3

- **修复**：网络 K 线导入两个缺陷——①清库后不建 `kline`/`adjust` 表（补 `CREATE STABLE IF NOT EXISTS`，`ImportKlineFromNetwork` 独立于 `DoImportTaos` 可用）；②遗漏复权因子（xdxr 0x0f）拉取，导致个股无除权除息事件（补 `NeedsAdjust` + `GetXdxr` + 增量写 `adjust` 表，恒瑞 600276 实证 85 条）。已合并入 `tdx import` 网络补缺路径。

### 2026-07-08 v0.14.2

- **修复**：`finance` 过滤条件过严——v0.14.0 用 `industry!=0 || 每股收益!=0` 把 ETF/基金/指数（这两个字段是个股专属、对它们恒为 0）全过滤了，但它们有股本+IPO 数据。改为「任一字段非 0 即入库」，finance 表从 15 只个股扩到含 ETF/指数/基金（42 只里 15 个股 + 27 基金/指数）。`f10` 本就对 ETF/指数/基金有完整数据（基金概况/基金经理/基金净值等），无 bug。

### 2026-07-08 v0.14.1

- **修复**：`index-info` 指数价格未 /100（现价显示 `397088` 应为 `3970.88` 点）——在 `deserialize_index_info` 根源层缩放，CLI 显示与 `idx_info` 入库一致；`unusual` 校验 market∈{0,1,2}（误传 `sh000001` 不再静默查 SZ）；`board-quotes` 校验 board_id 正整数 + Help 文案 `<code>`→`<board_id>`。

### 2026-07-08 v0.14.0

- **新功能**：`fetch-quotes` 默认采集自选股 `zxg.blk`（`--all_market` 全市场、`--quote_codes` 显式优先、`TDX_ZXG_BLK` 环境变量覆盖路径）；入库与 mmap 共享同一采集范围。
- **修复**：`gbk_to_utf8` 把 iconv 的 `E2BIG`（输出缓冲满）误当致命错 `break`，导致任何 >256 字节输出的 GBK（如 F10 全文 23KB）被截断——改为流式 `continue` 续转；`IsQuoteTarget` 不认 `sh/sz/bj` 前缀（v0.13.8 后 `--quote_codes` 全被过滤）；finance/F10 误锁 `wi==0` 单 worker（只采 1/N 票）；finance 过滤指数/ETF 全 0 空壳记录。

### 2026-07-03 10:43:12
```
 .claude/PRPs/prds/phase6-intraday-shm-design.md    | 563 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 .claude/PRPs/reviews/phase6-intraday-shm.review.md | 280 ++++++++++++++++++++++++++++++++++++++++++++++++++
 CLAUDE.md                                          |   1 +
 CMakeLists.txt                                     |   2 +-
 README.md                                          |   5 +-
 include/tdx/shm/payload.hpp                        |  67 ++++++++++++
 include/tdx/shm/segment.hpp                        |  69 +++++++++++++
 include/tdx/shm/snapshot.hpp                       |  53 ++++++++++
 include/tdx/types.hpp                              |   1 +
 src/CMakeLists.txt                                 |   9 +-
 src/cli/fetch_quotes.cpp                           | 167 +++++++++++++++++++-----------
 src/cli/quotes_reader.cpp                          |  53 ++++++++++
 src/shm/CMakeLists.txt                             |   9 ++
 src/shm/segment.cpp                                | 118 +++++++++++++++++++++
 src/shm/snapshot.cpp                               |  73 +++++++++++++
```
