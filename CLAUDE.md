# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

用 C++ 单库实现通达信（TDX）行情数据读取，三层合并为同源（协议层 / 行情接口层 / 数据管理层），功能对齐父目录三个上游 Python 项目：

| 上游 | 路径 | 对应层 | 职责 |
|---|---|---|---|
| **opentdx** | `/home/li/peiking88/opentdx` | 协议层（最底） | TCP socket、二进制帧编解码、命令号注册表、本地 `.day/.lc1/.lc5`、服务器列表与并发测速 |
| **mootdx** | `/home/li/peiking88/mootdx` | 行情接口层 | 工厂分发（标准/扩展）、语义化 API、财务下载解析、复权因子、板块解析 |
| **tdxdata** | `/home/li/peiking88/tdxdata` | 数据管理层（最上） | 统一 `TdxData` API、熔断器+重试、增量同步、本地优先+网络补缺混合源、自算复权、A 股时段感知重采样 |

依赖链 `tdxdata → mootdx → opentdx`。C++ 版合并为单库，分层同源，不再跨语言依赖。本文件是 C++ 实现的设计蓝图与协议知识库。

**完成状态**：Phase 1-6 全部完成（v0.15.6）。

**里程碑**（详见 git log / README changelog）：
- **Phase 1-3（v0.1-0.3）**：协议层 + A股标准/扩展/SP/MAC 行情 + 数据管理核心（Calendar/Adjust/Resampler/SyncState/TdxData）
- **Phase 4-5（v0.4-0.9.2）**：v2 改进（external/ 统一、断点续传、并发批量）+ abseil time_util + 离线构建 + e2e 真网测试 + TDengine 多线程导入 + XDXR 除权除息 + 代码名称三件套 + 导入过滤（北交所/ETF/LOF/板块指数）
- **v0.10-0.12**：字段缩放统一（`scaling.hpp`）+ 盘中实时数据落库 + 补全 8 个盘中接口 + thread-affinity 修复 + F10 文本入库 + SQL 注入防护（`IsValidCode`）
- **v0.13（Phase 6）**：盘中实时行情共享内存——`fetch-quotes` 改「获取-缓存(mmap)-异步入库」（`--mmap_path`），新增 `tdx_shm` 模块（裸 mmap + seqlock 快照表，`/dev/shm` 跨进程），采集节拍与入库解耦。设计见 `.claude/PRPs/prds/phase6-intraday-shm-design.md`
- **v0.14**：`fetch-quotes` 默认采集自选股 `zxg.blk` + `fetch-finance`/`fetch-f10` 分离为独立命令 + `deserialize_finance` 协议偏移修复
- **v0.15**：心跳 dispatcher 崩溃修复（blocker，`OnTimer` 包 `MakeFiber`）+ fetch-kline 日志去重/非交易日早退 + fetch-quotes 三处修复
- **v0.15.3**：文档整理——CLAUDE.md 全面重组（补 shm 模块/命令/心跳教训，里程碑压缩）+ README.md 删垃圾 diff 块/修过时 flag
- **v0.15.4**：移除已回退命令的 CLI 分发（batch-fetch/bars/ex-bars/fetch-history/pull-kline 全部落入未知命令；tdx 链接移除 tdx_batch）+ 文档清理（README/CLAUDE 移除 batch-fetch/quotes_reader 引用）
- **v0.15.5**：新增编排脚本 `scripts/fetch-today.py`——并行启动 fetch-kline + fetch-quotes --quote_loop，每分钟报告进度；codes 来自 zxg.blk / --codes / --codes-file，可选 --mmap
- **v0.15.6**：修复 fetch-today.py——codes-file 支持空格/换行分隔；stderr 改 mkstemp 单一 fd + stdbuf 行缓冲（防 abort 丢 trace）+ 环形缓冲 dump；Ctrl-C 8s grace 优雅退出

**上游短板改进**（C++ 版相对 Python 上游）：①并发批量下载（`tdx_batch` helio fiber 池 + `-n`）；②断点续传（`SyncState` JSON 持久化）；③统一 TDengine 时序存储（替代上游零散存储）。

## 通达信协议核心知识（移植时必须逐字对照）

这是 C++ 重写最大的工作量与风险点，全部源自上游实测：

### 连接
- **传输层**：原生 TCP socket（非 HTTP），默认 `time_out=5` 秒，IPv4/IPv6 自动选择。参考 `opentdx/client/baseStockClient.py:138-227`。
- **端口**：标准行情 `7709`；扩展行情（期货/港美股）`7727`；SP/MAC 高级行情（板块/资金流）走 `mac_hosts` 也用 `7709`。参考 `opentdx/const.py:4-193`。
- **登录**：连接后必须发送 `Login`（msg_id `0x0d`），请求体 `struct.pack('<B', 1)`。SP 模式登录是不同的 msg_id `0x2454`。参考 `opentdx/parser/server.py`。
- **心跳**：`HeartBeatThread`，**15 秒间隔**（`DEFAULT_HEARTBEAT_INTERVAL=15.0`），连续 20 次纯心跳无业务请求则主动断开。每次业务请求必须重置该计数。参考 `opentdx/utils/heartbeat.py:10-46`。
- **自动选服**：未指定 IP 时并发（上游用 `ThreadPoolExecutor` max 10）测所有 host 的 TCP 连接延迟，选最快者。参考 `opentdx/client/baseStockClient.py:138-177`、`mootdx/server.py:42-71`。
- **自动重试**：上游策略 `[0.1, 0.5, 1, 2]` 秒共 4 次，每次重连重发。参考 `opentdx/client/baseStockClient.py:78-87`。

### 二进制帧格式（全小端 `<`）
- **请求头 12 字节**：`struct.pack('<BIBHH', head, customize, control=1, lbody, lbody)`，紧跟 body = `struct.pack('<H', msg_id) + payload`。`head`：`0x0c`=不压缩 / `0x1c`=请求压缩。参考 `opentdx/parser/baseParser.py:9-25`。
- **响应头 16 字节**（`RSPHEADER_LEN=0x10`）：`struct.unpack('<IBIBHHH', ...)` → prefix(4B，固定 `b1 cb 74 00`) / zipped / customize / unknown / msg_id / zipsize / unzip_size。当 `zipsize != unzip_size` 时 body 用 **zlib 解压**（注意：是 zlib，不是 lzma）。参考 `opentdx/client/baseStockClient.py:283-321`。
- **中文编码**：**GBK** 解码（如股票名称、公告）。
- **价格缩放**：实时行情价格需 `/100`，金额 `*100`；财务字段常以「万元」为单位需 `*10000` 还原。参考 `opentdx/client/quotationClient.py:24-30`、`mootdx/hq_adapter.py:293-294`。

### 非平凡编码（务必仔细移植，易错）
- **变长价格 `get_price`**：类 UTF-8 变长有符号整数——`bit 0x40`=符号位、`bit 0x80`=继续位、每后续字节 7 bit。参考 `opentdx/utils/help.py:137-169`。
- **紧凑日期 `to_datetime`**：日 K 用 `YYYYMMDD` 整数；分钟线用紧凑编码（低 16 位 = `(year-2004)<<11 | month*100+day`，高 16 位 = 当日分钟数）。参考 `opentdx/utils/help.py:171-207`。
- **K 线请求体**：`struct.pack('<H6sHHHHH8s', market, code.gbk, period, times, start, count, adjust, b'')`——注意 code 是 **6 字节 GBK**。参考 `opentdx/parser/quotation/kline.py:12`。

### 命令号（msg_id）分区
C++ 用注册表（`msg_id → 解析器`）实现。上游用 `@register_parser(msg_id, head, customize, need_zip)` 装饰器，见 `opentdx/parser/`。主要分区：
- **A股行情**（`head=0x0c, customize=0`）：`0x04` 心跳、`0x0d` 登录、`0x0f` 除权除息、`0x10` 财务、`0x44d` 列表、`0x44e` 数量、`0x523` K线（核心）、`0x537` 分时、`0x53e` 详细报价、`0x53f` 排行榜、`0x563` 主力异动、`0x56a` 集合竞价、`0xfc5` 实时逐笔、`0xfeb` 历史分时、`0xfb4/0xfb5` 历史委托/成交、`0x2cf/0x2d0` F10。
- **扩展行情**（`customize=1`）：`0x23f4/0x23f5` 类别/列表、`0x23ff/0x2489` K线、`0x23fa` 单报价、`0x2412` 历史成交、`0x2454` 登录。
- **SP/MAC 协议**（`customize=1 或 2`）：`0x1218` 资金流向、`0x122C` 板块成员报价、`0x122E` 板块K线、`0x1231` 板块列表、`0x1237` 异动、`0x123D` 竞价。SP 字段用位图映射，见 `opentdx/utils/bitmap.py`。

### 本地 vipdoc 文件格式
读取通达信本地目录（环境变量 `TDX_HOME` 或默认安装路径）：
- `.day` 日线：`<IffffIIf`（扩展行情多一列 hk_stock_amount）。参考 `opentdx/reader/daily_bar_reader.py:35`。
- `.lc1` 1分钟线 / `.lc5` 5分钟线：均为 `<HHfffffII`（32 字节/条，OHLC=float 直读）。`opentdx/reader/min_bar_reader.py` 的 `<HHIIIIfII` 注释已过时（实测 hex 为 float）。参考 `opentdx/reader/lc_min_bar_reader.py`。
- 路径模式：`{tdxdir}/vipdoc/{sh|sz|bj|ds}/{lday|fzline|minline}/{symbol}.{day|lc5|lc1|5|1}`。`88****` 板块指数放 `sh` 目录；`#` 开头为扩展市场 `ds`。
- 板块文件 `block.dat` 等固定 2800 字节/块。参考 `opentdx/utils/block_reader.py:40-70`。

## 行情接口范围（功能对齐清单）

- **标准行情（A股，端口 7709）**：K线（含前/后复权）、分时图、逐笔成交、五档报价、股票列表/数量、除权除息、财务、F10、指数、成交量分布。
- **扩展行情（期货/港美股/期权/债券，端口 7727）**：商品列表、报价、K线、历史成交、分时。
- **SP/MAC 高级行情**：板块列表/成员、资金流向、集合竞价、异动监控（需 `client.sp()` 切换到 mac_hosts）。
- **本地数据**：`.day/.lc1/.lc5` 读取与时效性校验。
- **数据管理（tdxdata 层）**：统一 API、熔断器+指数退避重试、增量同步（JSON 状态持久化）、本地优先+网络补缺的混合数据源、自算复权因子、A 股交易时段感知的 K 线重采样、标准化输出 schema。

### 本地数据导入与过滤

`tdx import` 从本地 vipdoc 目录导入日线到 TDengine，通过 `IsAStock()` 过滤代码：

| 包含 | 代码段 | 说明 |
|---|---|---|
| 深市主板/中小 | `0xxxxx` | A股 |
| 创业板 | `3xxxxx` | 含深证指数 `399xxx` |
| 沪市主板/科创板 | `6xxxxx` | A股 |
| 北交所 | `4xxxxx`, `8xxxxx` | 不含 `88` 板块指数 |
| 板块/沪深指数 | `88xxxx` | 通达信板块指数 |
| 沪市 ETF/LOF | `5xxxxx` | 基金 |
| 深市 ETF | `159xxx` | 基金 |
| 深市 LOF | `16xxxx` | 基金 |
| **排除** | | |
| 债券 | `1xxxxx`(非159/16) | 含可转债 |
| B股/债券 | `2xxxxx` | |
| 港股通 | `7xxxxx` | |
| B股 | `9xxxxx` | |

对照表 `stock_names.json` 由 `fetch-names` 建立，`check-names` 校验完整性，`cleanup` 清理对照表中已不再覆盖的冗余条目。

## 技术栈

- **构建**：CMake + Ninja。**C++ 标准定为 C++17**。
- **版本号**：写在 `CMakeLists.txt` 的 `project(tdx-cpp VERSION x.y.z)`，遵循全局规范的打 tag 规则（不加 `v` 前缀）。
- **异步 IO**：**helio**（`external/helio`，由 `scripts/setup_external.sh` 从 `~/framework/dragonfly/helio` rsync 复制源码（排除 build/.git/），io_uring+epoll+自研 fiber 协程，C++17）。`AddPeriodic` 做 15s 心跳、`MakeFiber` 做并发测速、`FiberSocketBase` 做 TCP。参考 `helio/examples/echo_server.cc` 的 `Driver`/`TLocalClient`。无 install target，须 `add_subdirectory(external/helio)` 内嵌；需系统 Boost(context+system)，其余依赖(abseil/glog/liburing 等)自动拉。
- **GBK 转码**：系统 iconv。
- **压缩**：zlib（协议必需）。
- **测试**：GoogleTest + CTest，live 测试间加延迟退避避免限流。
- **存储**：TDengine 时序数据库（多线程并发导入）。

## 目录结构与模块架构

```
docs/        API 文档、协议说明、PRD
cfg/         配置文件（servers.json / holidays.json）
include/     公共头文件（tdx/ 命名空间）
  ├─ tdx/types.hpp           KLine/Tick/Transaction/Quote/Stock 结构体
  ├─ tdx/consts.hpp          协议常量、市场/周期/交易时段枚举、ParseMarketCode
  ├─ tdx/errors.hpp          异常类型
  ├─ tdx/proto/              协议层头文件（13 个）
  ├─ tdx/quotes/             行情接口层头文件（3 个：std/ext/sp）
  ├─ tdx/data/               数据管理层头文件（adjust/calendar/resampler/scaling/sync_state/tdx_data）
  ├─ tdx/taos/               TDengine 导入层头文件
  ├─ tdx/batch/              批量拉取头文件
  ├─ tdx/shm/                盘中实时共享内存（payload/segment/snapshot）
  ├─ tdx/util/               工具头文件（5 个：gbk/zlib_wrap/time_util/byte_order/code_validate）
  └─ nlohmann/               JSON 库（vendored 单头文件）
src/         源码
  ├─ util/                   gbk / zlib_wrap / time_util（absl::Time+FixedTimeZone）
  ├─ proto/                  协议层（14 文件，拆 4 子 target：core/transport/parsers/local）
  ├─ quotes/                 行情接口层（std/ext/sp）
  ├─ data/                   数据管理层
  ├─ taos/                   TDengine 导入层
  ├─ batch/                  并发批量拉取
  ├─ shm/                    盘中实时共享内存（segment/snapshot）
  ├─ cli/                    CLI 入口（main.cpp / fetch_quotes.cpp / import.cpp）
  └─ smoke.cc                烟雾测试
scripts/     辅助脚本（setup_external.sh / record_golden.py / reimport.py）
tests/       单元测试 + 集成测试 + fixtures/golden/（黄金字节流真服录制）
external/    第三方依赖（不入 git，setup_external.sh 初始化）
output/      程序输出（不入 git）
```

| 命名空间 | CMake target | 职责 |
|---|---|---|
| `tdx::util` | `tdx_util` | GBK iconv、zlib 解压、**absl::Time+FixedTimeZone(+8)** 替代 POSIX timegm/gmtime_r、字节序、代码校验 |
| `tdx::proto` | `tdx_proto`（umbrella INTERFACE） | 协议层：帧编解码（core）、连接/心跳/熔断/选服（transport）、解析器（parsers）、本地文件（local） |
| `tdx::quotes` | `tdx_quotes` | 行情接口层（StdQuotes / ExtQuotes / SpQuotes） |
| `tdx::data` | `tdx_data` | 数据管理层（Calendar / Adjust / Resampler / SyncState / TdxData） |
| `tdx::taos` | `tdx_taos` | TDengine 导入层（多线程 + 批量 INSERT） |
| `tdx::batch` | `tdx_batch` | 并发批量拉取（helio fiber 池分片 + `-n` 并发数） |
| `tdx::shm` | `tdx_shm` | 盘中实时行情共享内存（mmap 段 + seqlock 快照表，`/dev/shm` 跨进程 O(1) 读最新价） |
| `tdx` (exe) | `tdx` | CLI 入口（见下） |

**CLI 命令**（`src/cli/main.cpp` 分发）：
- **采集/入库**：`server-test`（测速选服）、`import`（vipdoc 历史导入）、`fetch-quotes`（实时行情→TDengine/mmap）、`fetch-kline`（当日K线循环）、`fetch-finance`/`fetch-f10`（财务/F10 独立重导）、`truncate-quotes`（清当日盘中队列）
- **盘中接口**：`history-orders`、`history-tx`、`vol-profile`、`index-info`、`unusual`、`board-list`、`board-quotes`、`capital-flow`
- **代码名称管理**：`fetch-names`、`cleanup`、`check-names`
- **批量**：`tdx_batch`（src/batch/，BatchFetchKline 用于并发测试，已从 CLI 移除）
- **已移除命令**：`pull-kline`（换 `import`）；`bars`/`ex-bars`/`fetch-history`/`batch-fetch` 已从 CLI 移除（`batch-fetch` 回落为测试用例）

## helio fiber 编码纪律（关键约束）

helio 的 Proactor 线程内**禁用**标准库阻塞原语——它们会阻塞整个事件循环线程（不只当前 fiber）：

| 禁用 | 替代（`util::fb2::`） |
|---|---|
| `std::mutex` / `std::lock_guard` | `util::fb2::Mutex` |
| `std::condition_variable` | `util::fb2::CondVar` / `EventCount` / `Done` |
| `std::this_thread::sleep_for` | `ThisFiber::SleepFor` |

来源：`helio/CLAUDE.md:67-77`。

**适用范围**：所有在 helio Proactor 线程 / fiber 上下文内运行的代码——主要是 `tdx::proto` 协议层全部，以及任何被 fiber 调用的逻辑。写这些代码时必须应用本纪律。

**测试代码豁免**（评审决策 #3）：gtest 测试框架与测试编排运行在 Proactor 线程之外（main / gtest worker 线程），**豁免**此纪律。但：① 被测的 fiber 内业务代码仍须遵守；② 测试若需在 Proactor 线程内编排（如等待 fiber 完成），用 `ProactorPool::Await` / `Fiber::Join`，不要裸 `std::this_thread::sleep_for`。

**第三方库审计**：在 fiber 内调用第三方库前，确认其内部无阻塞原语——iconv / zlib 安全；nlohmann_json / spdlog / fmt 需确认，或避免在 fiber 内调用（改在 fiber 外做 I/O，或用 fb2 同步原语保护）。

## 关键实现注意事项

- **thread-affinity**：helio 要求 Proactor/socket/Periodic 操作**必须在所属 Proactor 线程执行**。StdQuotes 内部方法（Bars/Quotes 等）已包 `proactor_->Await`，`Close()` 的 heartbeat Stop（CancelPeriodic）+ conn Close（socket）也必须包 Await。`SPQuotes::Call` 必须包 Await（raw `conn_->Call` 在主线程调违规）。Debug 下 `DCHECK(InMyThread)` abort，Release 静默 hang/SEGFAULT。新加任何 Proactor/fiber 操作用 `proactor_->Await` 或 `pb->Await` 包装。
- **定时器回调不可同步 Suspend（v0.15.1 教训）**：`AddPeriodic` 注册的回调跑在 helio **periodic dispatcher fiber** 上，该 fiber 不允许 `Suspend`。若回调内调 `conn_->Call`（会 Suspend）→ `Should not preempt dispatcher` abort。`Heartbeat::OnTimer` 的 `send_fn`/`timeout_fn` 须包 `::util::MakeFiber(...).Detach()` 切到普通 fiber 执行。
- **`Close()` 销毁顺序**：`server_pool_` 持有 `pool_` 裸指针，须在 `pool_->Stop()` 前 `server_pool_.reset()`。
- **`SelectBest` 禁止跨调度器**：`MakeFiber` 后禁止 `GetNextProactor()->Await`——所有探测 fiber 用单一 `pb`。
- **字段缩放**：`include/tdx/data/scaling.hpp`（纯头文件）——`SecurityClass × DataSource → FieldScaling`。新增 parser/数据源必走此表。
- **SP 测试**：独立二进制 `test_sp_e2e`（同进程两个 ProactorPool 增加 fiber 调度复杂度）。
- **精度**：遵循全局规范——价位/金额 `%.2f`、数量 `%d`、百分比 `%d%%`。
- **市场前缀**：所有 code 必须带市场前缀 `sh`/`sz`/`bj`（如 `sh000001`、`sz000001`、`bj430047`），用 `tdx::ParseMarketCode` 解析；无前缀视为无效（不回退 `MarketFromCode` 推断）。歧义 code（`000001` SH=上证指数 / SZ=平安银行）须显式前缀区分。CLI 命令、fetch-quotes 均要求前缀；导入内部 `BatchNetImport` 用 `stock_name.market` 字段而非 `MarketFromCode`。
- **复权**：tdxdata 的复权因子是**基于 xdxr 事件流自行计算**（`tdxdata/sources/adjust.py:49`，区分前复权 qfq 的 backward-asof 与后复权 hfq 的 forward-asof），不是直接取交易所因子。移植时须对照其单测。
- **A 股时段感知重采样**：15m/30m/1h 由 5m 重采样，1w/1mon 由 1d 重采样，且 K 线结束时间须按 A 股交易时段（上午 9:30、下午 13:00 开盘）标注。参考 `tdxdata/sources/base.py:56-132`。
- **K 线周期常量**：`0`=5min、`1`=15min、`2`=30min、`3`=1h、`4`=日、`5`=周、`6`=月、`7`=扩展1min、`8`=1min、`9`=日K、`10`=季、`11`=年。单次请求 K 线上限 800 条、分笔 2000 条。
- **结构体移植**：所有 `struct.unpack` 格式串须逐字移植，不可臆测字节序。
- **时区转换**：`time_util` 用 `absl::Time` + `absl::FixedTimeZone(+8)` 替代 POSIX `timegm`/`gmtime_r`——消除非标准依赖、天然线程安全。见 `src/util/time_util.cpp`。
- **离线构建**：`scripts/setup_external.sh` 预下载 googletest/benchmark/abseil-cpp/gperf/xxhash/liburing 等依赖的 tarball 到 `external/`，CMake FetchContent 使用 `file://` URL 离线使用，构建期无需联网。

## 构建、测试、运行

```bash
# 0. 首次初始化 external/ 依赖（helio 源码 + 预下载 tarball，支持离线构建）
bash scripts/setup_external.sh

# 1. 配置 + 编译（Release，ninja，-j$(nproc) 并行）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 2. 测试（ctest 并行）
ctest --test-dir build -j$(nproc) --output-on-failure
ctest --test-dir build -R <test_name> -V   # 单个用例

# 3. CLI 示例
./build/bin/tdx server-test                              # 测速选服
./build/bin/tdx import --tdx-root ~/.wine/.../tdx        # 本地 vipdoc 导入 TDengine
./build/bin/tdx fetch-kline sh600000 1d 240              # 当日K线循环刷新入库
./build/bin/tdx fetch-quotes --mmap_path /dev/shm/tdx    # 实时行情→mmap+异步入库
./build/bin/tdx fetch-finance sh600000                   # 财务数据入库
./build/bin/tdx fetch-names && tdx check-names           # 代码→名称对照表
```

**真实网络测试**（连接通达信服务器）优先级高于 mock，参考上游的 `pytest -m live`（连真服）与 `-m local`（读本地文件）划分。e2e 测试（`test_e2e`）在服务器不可达时自动 `GTEST_SKIP`，不视为失败。

## 数据导入架构

**历史 K 线 = vipdoc，当日盘中 = 网络**：

| 数据来源 | 命令 | 周期 | 范围 |
|---|---|---|---|
| 本地 vipdoc | `tdx import` | 1d/1m/5m | 历史（今日之前） |
| 网络 | `tdx fetch-kline` | 1d/5m/1m | 当日盘中 |

- `tdx import` 默认仅导自选股 `zxg.blk`（与 `fetch-quotes` 一致），加 `--all-market` 导全市场
- 导入时自动 DROP+重建 1d/1m/5m 子表，清除网络旧数据（含解析错位产生的 23:55/16:39 脏 bar）
- Parser 层 D7 校验：分钟 bar 须在 9:30–11:30 或 13:00–15:00，否则丢弃
- `fetch-kline` 仅保留当日 bar + 交易时段校验 + OHLC 正数/high≥low

## 参考资源

- 协议层实现样板：`/home/li/peiking88/opentdx/parser/baseParser.py`（封包）、`opentdx/client/baseStockClient.py:283-321`（收发+解压）。
- 全量命令号与服务器列表：`/home/li/peiking88/opentdx/const.py`。
- 统一 API 与熔断/增量设计：`/home/li/peiking88/tdxdata/api.py`、`tdxdata/core/data_manager.py`、`tdxdata/errors.py`、`tdxdata/sync.py`。
- 复权计算与时段重采样：`/home/li/peiking88/tdxdata/sources/adjust.py`、`base.py`。
- 用户全局开发规范：`~/.claude/CLAUDE.md`（Git 认证、中文、编译并行参数、测试覆盖率>80%、精度格式等，本项目一并遵循）。
